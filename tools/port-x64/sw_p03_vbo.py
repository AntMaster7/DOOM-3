"""idVertexCache: a header from the block allocator is not zeroed, and in virtual-memory mode nothing
ever set its vbo: Position() then returned an OFFSET for a garbage buffer name. Latent in the
original (fresh heap pages happened to be zero); the software renderer always runs this mode."""
import sys
sys.path.insert(0, 'C:/Source/DOOM-3/tools/port-x64')
from px import patch

patch('renderer/VertexCache.cpp', [
("""			if( !virtualMemory ) {
				qglGenBuffersARB( 1, & block->vbo );
			}
""", """			block->vbo = 0;		// the allocator does not clear; without this, virtual memory mode read garbage
			block->virtMem = NULL;
			if( !virtualMemory ) {
				qglGenBuffersARB( 1, & block->vbo );
			}
"""),
])
