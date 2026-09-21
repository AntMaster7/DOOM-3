/* sw_present_native.cpp -- Stage 9: the finished frame onto the screen WITHOUT OpenGL.

   Three ways, tried in this order, reported once in the console:

   1 "zero copy"  D3D12. A framebuffer of the core is one VirtualAlloc region of whole tiles, so
                  ID3D12Device3::OpenExistingHeapFromAddress turns the region itself into a heap
                  and the GPU copies the frame out of it over PCIe. The CPU records one copy
                  command.
   2 "upload"     D3D12 with upload buffers that sw_copy_frame fills in the pool.
   3 "gdi"        SetDIBitsToDevice, for a machine without D3D12.

   The price of 1 is a race: the GPU reads a frame while the next one would be written over it,
   and WHEN the GPU gets to the copy is not ours to decide (it is shared with the compositor and
   everything else on the desktop: measured at 3840x2160 with one framebuffer, the wait for the
   copy was 2.65 ms per frame on average and 20 ms at worst). So the core renders into two
   framebuffers in turn (sw_framebuffer_select), each with its own source, command allocator and
   fence value, and SWN_BeginFrame waits only for the last copy out of the framebuffer that is
   about to be written: a frame's worth of slack.

   No engine headers in here on purpose: d3d12.h and idlib do not need to meet. The engine side
   is sw_present.cpp. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_5.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sw_api.h"
#include "sw_present_native.h"

#pragma comment( lib, "d3d12.lib" )
#pragma comment( lib, "dxgi.lib" )
#pragma comment( lib, "gdi32.lib" )

enum { SWN_NONE, SWN_ZEROCOPY, SWN_UPLOAD, SWN_GDI };
#define SWN_BUFFERS 2		// of the swap chain
#define SWN_SOURCES SW_MAX_FRAMEBUFFERS

typedef struct {
	const void *				base;			// the framebuffer region this source stands for
	ID3D12Heap *				heap;			// zero copy: the region's own pages, as the GPU sees them
	ID3D12Resource *			buffer;			// zero copy: placed in `heap`; upload: committed
	void *						uploadPtr;
	ID3D12CommandAllocator *	allocator;		// reset only when this source's last copy is done
	UINT64						fenceValue;		// of that copy
} swnSource_t;

static struct {
	int							mode;
	HWND						hwnd;
	int							width, height, pitch;
	size_t						fbBytes;
	bool						tearing;
	bool						vsync;			// built for a swap interval: one frame in flight, waited for
	bool						zeroCopy;

	IDXGIFactory4 *				factory;
	ID3D12Device *				device;
	ID3D12CommandQueue *		queue;
	IDXGISwapChain3 *			swapChain;
	ID3D12Resource *			backBuffers[SWN_BUFFERS];
	ID3D12GraphicsCommandList *	list;
	ID3D12Fence *				fence;
	UINT64						fenceValue;
	HANDLE						fenceEvent;
	HANDLE						latencyHandle;
	swnSource_t					sources[SWN_SOURCES];

	// where present time goes: [0] the fence wait before a frame's tiles, [1] recording + Present()
	double						waitMs[2], waitMax[2];
	int							frames, slow[2];		// slow: above 1 ms

	char						msg[1024];
} swn;

#define SWN_RELEASE( p ) do { if ( p ) { ( p )->Release(); ( p ) = NULL; } } while ( 0 )

static void SWN_Log( const char *fmt, ... ) {
	const size_t used = strlen( swn.msg );
	va_list ap;
	va_start( ap, fmt );
	vsnprintf( swn.msg + used, sizeof( swn.msg ) - used, fmt, ap );
	va_end( ap );
}

const char *SWN_Message( void ) {
	return swn.msg;
}

const char *SWN_ModeName( void ) {
	static const char *names[] = { "none", "D3D12, zero copy", "D3D12, upload buffer", "GDI" };
	return names[swn.mode];
}

static double SWN_Ms( void ) {
	static LARGE_INTEGER freq;
	LARGE_INTEGER t;
	if ( !freq.QuadPart ) {
		QueryPerformanceFrequency( &freq );
	}
	QueryPerformanceCounter( &t );
	return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static void SWN_Account( int which, double ms ) {
	swn.waitMs[which] += ms;
	if ( ms > swn.waitMax[which] ) {
		swn.waitMax[which] = ms;
	}
	if ( ms > 1.0 ) {
		swn.slow[which]++;
	}
}

const char *SWN_Timing( void ) {
	static char line[256];
	const double n = swn.frames ? (double)swn.frames : 1.0;
	snprintf( line, sizeof( line ), "present, %d frames: wait for the GPU's copy mean %.2f max %.1f ms (%d above 1 ms); record + Present mean %.2f max %.1f ms (%d above 1 ms)",
		swn.frames, swn.waitMs[0] / n, swn.waitMax[0], swn.slow[0], swn.waitMs[1] / n, swn.waitMax[1], swn.slow[1] );
	return line;
}

static void SWN_WaitForFence( UINT64 value ) {
	if ( swn.fence && swn.fence->GetCompletedValue() < value ) {
		swn.fence->SetEventOnCompletion( value, swn.fenceEvent );
		WaitForSingleObject( swn.fenceEvent, 2000 );
	}
}

static void SWN_ReleaseSource( swnSource_t *s ) {
	if ( s->buffer && s->uploadPtr ) {
		s->buffer->Unmap( 0, NULL );
	}
	SWN_RELEASE( s->buffer );
	SWN_RELEASE( s->heap );
	SWN_RELEASE( s->allocator );
	memset( s, 0, sizeof( *s ) );
}

static void SWN_ReleaseD3D( void ) {
	SWN_WaitForFence( swn.fenceValue );
	for ( int i = 0; i < SWN_SOURCES; i++ ) {
		SWN_ReleaseSource( &swn.sources[i] );
	}
	SWN_RELEASE( swn.list );
	for ( int i = 0; i < SWN_BUFFERS; i++ ) {
		SWN_RELEASE( swn.backBuffers[i] );
	}
	if ( swn.latencyHandle ) {
		CloseHandle( swn.latencyHandle );
		swn.latencyHandle = NULL;
	}
	SWN_RELEASE( swn.swapChain );
	SWN_RELEASE( swn.fence );
	if ( swn.fenceEvent ) {
		CloseHandle( swn.fenceEvent );
		swn.fenceEvent = NULL;
	}
	SWN_RELEASE( swn.queue );
	SWN_RELEASE( swn.device );
	SWN_RELEASE( swn.factory );
	swn.fenceValue = 0;
}

/* Must run before the core frees or moves its framebuffers (sw_resize, sw_shutdown): the heaps
   stand for those very pages. */
void SWN_Shutdown( void ) {
	SWN_ReleaseD3D();
	swn.mode = SWN_NONE;
	swn.hwnd = NULL;
}

static D3D12_RESOURCE_DESC SWN_BufferDesc( size_t bytes, D3D12_RESOURCE_FLAGS flags ) {
	D3D12_RESOURCE_DESC d;
	memset( &d, 0, sizeof( d ) );
	d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	d.Width = bytes;
	d.Height = 1;
	d.DepthOrArraySize = 1;
	d.MipLevels = 1;
	d.Format = DXGI_FORMAT_UNKNOWN;
	d.SampleDesc.Count = 1;
	d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	d.Flags = flags;
	return d;
}

/* a framebuffer region itself as a GPU-readable buffer */
static bool SWN_SourceZeroCopy( swnSource_t *s, const void *base ) {
	ID3D12Device3 *device3 = NULL;
	if ( FAILED( swn.device->QueryInterface( IID_PPV_ARGS( &device3 ) ) ) ) {
		SWN_Log( "  no ID3D12Device3\n" );
		return false;
	}
	HRESULT hr = device3->OpenExistingHeapFromAddress( base, IID_PPV_ARGS( &s->heap ) );
	device3->Release();
	if ( FAILED( hr ) ) {
		SWN_Log( "  OpenExistingHeapFromAddress: 0x%08lX\n", (unsigned long)hr );
		return false;
	}
	const D3D12_HEAP_DESC hd = s->heap->GetDesc();
	if ( hd.SizeInBytes < swn.fbBytes ) {
		SWN_Log( "  the heap is %llu bytes, the framebuffer %llu\n", (unsigned long long)hd.SizeInBytes, (unsigned long long)swn.fbBytes );
		SWN_RELEASE( s->heap );
		return false;
	}
	const D3D12_RESOURCE_FLAGS flags = ( hd.Flags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER ) ? D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER : D3D12_RESOURCE_FLAG_NONE;
	const D3D12_RESOURCE_DESC bd = SWN_BufferDesc( swn.fbBytes, flags );
	// a buffer in COMMON is promoted to COPY_SOURCE implicitly; try the explicit state first
	static const D3D12_RESOURCE_STATES states[2] = { D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON };
	for ( int i = 0; i < 2; i++ ) {
		hr = swn.device->CreatePlacedResource( s->heap, 0, &bd, states[i], NULL, IID_PPV_ARGS( &s->buffer ) );
		if ( SUCCEEDED( hr ) ) {
			return true;
		}
	}
	SWN_Log( "  CreatePlacedResource in the framebuffer heap (heap flags 0x%X): 0x%08lX\n", (unsigned)hd.Flags, (unsigned long)hr );
	SWN_RELEASE( s->heap );
	return false;
}

static bool SWN_SourceUpload( swnSource_t *s ) {
	D3D12_HEAP_PROPERTIES hp;
	memset( &hp, 0, sizeof( hp ) );
	hp.Type = D3D12_HEAP_TYPE_UPLOAD;
	const D3D12_RESOURCE_DESC bd = SWN_BufferDesc( swn.fbBytes, D3D12_RESOURCE_FLAG_NONE );
	HRESULT hr = swn.device->CreateCommittedResource( &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, IID_PPV_ARGS( &s->buffer ) );
	if ( FAILED( hr ) ) {
		SWN_Log( "  upload buffer: 0x%08lX\n", (unsigned long)hr );
		return false;
	}
	const D3D12_RANGE noRead = { 0, 0 };
	hr = s->buffer->Map( 0, &noRead, &s->uploadPtr );
	if ( FAILED( hr ) ) {
		SWN_Log( "  mapping the upload buffer: 0x%08lX\n", (unsigned long)hr );
		SWN_RELEASE( s->buffer );
		return false;
	}
	return true;
}

/* the source of framebuffer `index`, made when that framebuffer is first presented */
static swnSource_t *SWN_Source( int index, const void *base ) {
	swnSource_t *s = &swn.sources[index];
	if ( s->buffer && s->base == base ) {
		return s;
	}
	SWN_WaitForFence( s->fenceValue );
	SWN_ReleaseSource( s );
	if ( FAILED( swn.device->CreateCommandAllocator( D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS( &s->allocator ) ) ) ) {
		return NULL;
	}
	if ( swn.zeroCopy && SWN_SourceZeroCopy( s, base ) ) {
		s->base = base;
		return s;
	}
	if ( swn.zeroCopy ) {
		// all sources are of one kind: from here on, upload buffers
		swn.zeroCopy = false;
		swn.mode = SWN_UPLOAD;
		for ( int i = 0; i < SWN_SOURCES; i++ ) {
			if ( i != index ) {
				SWN_WaitForFence( swn.sources[i].fenceValue );
				SWN_ReleaseSource( &swn.sources[i] );
			}
		}
	}
	if ( SWN_SourceUpload( s ) ) {
		s->base = base;
		return s;
	}
	SWN_ReleaseSource( s );
	return NULL;
}

static bool SWN_InitD3D( void ) {
	// both DLLs are delay-loaded: a machine without them must get to the GDI path, not to an exception
	if ( !LoadLibraryA( "dxgi.dll" ) || !LoadLibraryA( "d3d12.dll" ) ) {
		SWN_Log( "  d3d12.dll or dxgi.dll is missing\n" );
		return false;
	}
	HRESULT hr = CreateDXGIFactory2( 0, IID_PPV_ARGS( &swn.factory ) );
	if ( FAILED( hr ) ) {
		SWN_Log( "  CreateDXGIFactory2: 0x%08lX\n", (unsigned long)hr );
		return false;
	}
	swn.tearing = false;
	IDXGIFactory5 *factory5 = NULL;
	if ( SUCCEEDED( swn.factory->QueryInterface( IID_PPV_ARGS( &factory5 ) ) ) ) {
		BOOL allow = FALSE;
		if ( SUCCEEDED( factory5->CheckFeatureSupport( DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof( allow ) ) ) ) {
			swn.tearing = allow != FALSE;
		}
		factory5->Release();
	}
	// the default adapter drives the primary display: no cross-adapter copy at present time
	hr = D3D12CreateDevice( NULL, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS( &swn.device ) );
	if ( FAILED( hr ) ) {
		SWN_Log( "  D3D12CreateDevice: 0x%08lX\n", (unsigned long)hr );
		return false;
	}
	D3D12_COMMAND_QUEUE_DESC qd;
	memset( &qd, 0, sizeof( qd ) );
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	hr = swn.device->CreateCommandQueue( &qd, IID_PPV_ARGS( &swn.queue ) );
	if ( FAILED( hr ) ) {
		SWN_Log( "  CreateCommandQueue: 0x%08lX\n", (unsigned long)hr );
		return false;
	}

	DXGI_SWAP_CHAIN_DESC1 sd;
	memset( &sd, 0, sizeof( sd ) );
	sd.Width = (UINT)swn.width;
	sd.Height = (UINT)swn.height;
	sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;			// the core's byte order, end to end
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = SWN_BUFFERS;
	sd.Scaling = DXGI_SCALING_STRETCH;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	// With a swap interval: a waitable swap chain holding ONE frame in flight (the default queue of
	// three is three frames of input lag). Without one there is nothing to wait for.
	sd.Flags = ( swn.vsync ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0 ) | ( swn.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0 );
	IDXGISwapChain1 *chain1 = NULL;
	hr = swn.factory->CreateSwapChainForHwnd( swn.queue, swn.hwnd, &sd, NULL, NULL, &chain1 );
	if ( FAILED( hr ) ) {
		SWN_Log( "  CreateSwapChainForHwnd: 0x%08lX\n", (unsigned long)hr );
		return false;
	}
	hr = chain1->QueryInterface( IID_PPV_ARGS( &swn.swapChain ) );
	chain1->Release();
	if ( FAILED( hr ) ) {
		SWN_Log( "  no IDXGISwapChain3: 0x%08lX\n", (unsigned long)hr );
		return false;
	}
	swn.factory->MakeWindowAssociation( swn.hwnd, DXGI_MWA_NO_ALT_ENTER );		// the engine owns alt-enter (vid_restart)
	if ( swn.vsync ) {
		swn.swapChain->SetMaximumFrameLatency( 1 );
		swn.latencyHandle = swn.swapChain->GetFrameLatencyWaitableObject();
	}
	for ( UINT i = 0; i < SWN_BUFFERS; i++ ) {
		hr = swn.swapChain->GetBuffer( i, IID_PPV_ARGS( &swn.backBuffers[i] ) );
		if ( FAILED( hr ) ) {
			SWN_Log( "  GetBuffer: 0x%08lX\n", (unsigned long)hr );
			return false;
		}
	}

	// the list is created against a throw-away allocator; each present resets it onto its source's own
	ID3D12CommandAllocator *first = NULL;
	hr = swn.device->CreateCommandAllocator( D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS( &first ) );
	if ( SUCCEEDED( hr ) ) {
		hr = swn.device->CreateCommandList( 0, D3D12_COMMAND_LIST_TYPE_DIRECT, first, NULL, IID_PPV_ARGS( &swn.list ) );
	}
	if ( SUCCEEDED( hr ) ) {
		swn.list->Close();
		hr = swn.device->CreateFence( 0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS( &swn.fence ) );
	}
	SWN_RELEASE( first );
	if ( FAILED( hr ) ) {
		SWN_Log( "  command list or fence: 0x%08lX\n", (unsigned long)hr );
		return false;
	}
	swn.fenceEvent = CreateEventA( NULL, FALSE, FALSE, NULL );
	return true;
}

/* (Re)builds the presenter when the window, the size or the way changed.
   way: 0 = best available, 2 = no zero copy, 3 = GDI (the A/B switch r_swPresent). */
bool SWN_Setup( void *hwnd, int width, int height, int pitchPixels, size_t fbBytes, int way, bool vsync ) {
	static int lastWay = -1;
	if ( swn.mode != SWN_NONE && swn.hwnd == (HWND)hwnd && swn.width == width && swn.height == height &&
		 swn.fbBytes == fbBytes && way == lastWay && vsync == swn.vsync ) {
		return false;
	}
	SWN_Shutdown();
	lastWay = way;
	swn.msg[0] = 0;
	swn.hwnd = (HWND)hwnd;
	swn.width = width;
	swn.height = height;
	swn.pitch = pitchPixels;
	swn.fbBytes = fbBytes;
	swn.vsync = vsync;
	swn.zeroCopy = way != 2;
	if ( way != 3 && SWN_InitD3D() ) {
		swn.mode = swn.zeroCopy ? SWN_ZEROCOPY : SWN_UPLOAD;
		return true;
	}
	SWN_ReleaseD3D();
	swn.mode = SWN_GDI;
	return true;
}

/* Before the first tile of a frame is stored into framebuffer `index`: the GPU must be done with
   the last copy out of THAT framebuffer. With a swap interval, also the swap chain's frame
   latency wait (one frame in flight). */
void SWN_BeginFrame( int index, int swapInterval ) {
	if ( ( swn.mode != SWN_ZEROCOPY && swn.mode != SWN_UPLOAD ) || index < 0 || index >= SWN_SOURCES ) {
		return;
	}
	const double t0 = SWN_Ms();
	SWN_WaitForFence( swn.sources[index].fenceValue );
	SWN_Account( 0, SWN_Ms() - t0 );
	if ( swapInterval > 0 && swn.latencyHandle ) {
		WaitForSingleObjectEx( swn.latencyHandle, 1000, FALSE );
	}
}

static void SWN_Transition( ID3D12Resource *res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to ) {
	D3D12_RESOURCE_BARRIER b;
	memset( &b, 0, sizeof( b ) );
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = res;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = from;
	b.Transition.StateAfter = to;
	swn.list->ResourceBarrier( 1, &b );
}

static bool SWN_PresentD3D( const void *base, int index, int swapInterval ) {
	swnSource_t *s = SWN_Source( index, base );
	if ( !s ) {
		return false;
	}
	SWN_WaitForFence( s->fenceValue );					// its allocator is reset below (SWN_BeginFrame has waited already)
	const double t0 = SWN_Ms();
	swn.frames++;
	if ( s->uploadPtr ) {
		sw_copy_frame( s->uploadPtr, swn.pitch );
	}
	if ( FAILED( s->allocator->Reset() ) || FAILED( swn.list->Reset( s->allocator, NULL ) ) ) {
		return false;
	}
	ID3D12Resource *back = swn.backBuffers[swn.swapChain->GetCurrentBackBufferIndex()];
	SWN_Transition( back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST );

	D3D12_TEXTURE_COPY_LOCATION dst, src;
	memset( &dst, 0, sizeof( dst ) );
	memset( &src, 0, sizeof( src ) );
	dst.pResource = back;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src.pResource = s->buffer;
	src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	src.PlacedFootprint.Footprint.Width = (UINT)swn.width;
	src.PlacedFootprint.Footprint.Height = (UINT)swn.height;
	src.PlacedFootprint.Footprint.Depth = 1;
	src.PlacedFootprint.Footprint.RowPitch = (UINT)swn.pitch * 4;		// whole tiles: a multiple of 256 bytes, as D3D12 wants it
	swn.list->CopyTextureRegion( &dst, 0, 0, 0, &src, NULL );

	SWN_Transition( back, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT );
	if ( FAILED( swn.list->Close() ) ) {
		return false;
	}
	ID3D12CommandList *lists[1] = { swn.list };
	swn.queue->ExecuteCommandLists( 1, lists );
	const HRESULT hr = swn.swapChain->Present( (UINT)swapInterval, ( swapInterval == 0 && swn.tearing ) ? DXGI_PRESENT_ALLOW_TEARING : 0 );
	swn.queue->Signal( swn.fence, ++swn.fenceValue );
	s->fenceValue = swn.fenceValue;
	SWN_Account( 1, SWN_Ms() - t0 );
	return SUCCEEDED( hr );
}

static void SWN_PresentGDI( const uint32_t *pixels ) {
	struct {
		BITMAPINFOHEADER	h;
		DWORD				masks[3];
	} bi;
	memset( &bi, 0, sizeof( bi ) );
	bi.h.biSize = sizeof( bi.h );
	bi.h.biWidth = swn.pitch;
	bi.h.biHeight = -swn.height;						// top-down, as the frame is
	bi.h.biPlanes = 1;
	bi.h.biBitCount = 32;
	bi.h.biCompression = BI_BITFIELDS;					// the frame is RGBA, a plain 32-bit DIB would be BGRA
	bi.masks[0] = 0x000000FF;
	bi.masks[1] = 0x0000FF00;
	bi.masks[2] = 0x00FF0000;
	HDC dc = GetDC( swn.hwnd );
	SetDIBitsToDevice( dc, 0, 0, (DWORD)swn.width, (DWORD)swn.height, 0, 0, 0, (UINT)swn.height, pixels, (const BITMAPINFO *)&bi, DIB_RGB_COLORS );
	ReleaseDC( swn.hwnd, dc );
}

/* `pixels` is framebuffer `index` of the core */
void SWN_Present( const uint32_t *pixels, int index, int swapInterval ) {
	if ( swn.mode == SWN_NONE || index < 0 || index >= SWN_SOURCES ) {
		return;
	}
	if ( swn.mode != SWN_GDI ) {
		if ( SWN_PresentD3D( pixels, index, swapInterval ) ) {
			return;
		}
		// device removed or the like: GDI cannot draw under a flip-model swap chain, so let go of it
		SWN_Log( "D3D12 present failed; falling back to GDI\n" );
		SWN_ReleaseD3D();
		swn.mode = SWN_GDI;
	}
	SWN_PresentGDI( pixels );
}
