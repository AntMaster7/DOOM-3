# markPose: the user's heavy spots for tools\pose-census.ps1, one key press each. In the engine
# proper (both builds): it must work whatever renders.
import os
ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..', 'neo'))

def patch(rel, pairs):
    path = os.path.join(ROOT, rel)
    s = open(path, encoding='latin-1').read()
    for old, new in pairs:
        if new in s:
            continue
        assert s.count(old) == 1, (rel, s.count(old), old[:60])
        s = s.replace(old, new)
    open(path, 'w', encoding='latin-1', newline='').write(s)
    print('patched', rel)

patch('renderer/RenderSystem_init.cpp', [
("""void R_ScreenShot_f( const idCmdArgs &args ) {""",
"""/*
==================
R_MarkPose_f

markPose [note]: appends the map, the eye position and the view angles of the last primary view
to poses.txt in the save path, in the form tools\\pose-census.ps1 takes (setviewpos wants the eye
position and the yaw). Bind it to a key and press it wherever the frame rate hurts.
==================
*/
void R_MarkPose_f( const idCmdArgs &args ) {
	if ( !tr.primaryWorld ) {
		common->Printf( "markPose: no map is being rendered\\n" );
		return;
	}
	const renderView_t &v = tr.primaryRenderView;
	const idAngles angles = v.viewaxis.ToAngles();
	idStr map = tr.primaryWorld->mapName;
	map.StripFileExtension();
	map.StripLeading( "maps/" );
	const idStr line = va( "%s | %.1f %.1f %.1f %.1f | pitch %.1f | %ix%i | %s\\n", map.c_str(), v.vieworg.x, v.vieworg.y, v.vieworg.z,
		angles.yaw, angles.pitch, glConfig.vidWidth, glConfig.vidHeight, args.Argc() > 1 ? args.Args() : "" );
	idFile *f = fileSystem->OpenFileAppend( "poses.txt", true, "fs_savepath" );		// NOT the default, fs_basepath: that is the game's own directory
	if ( !f ) {
		common->Printf( "markPose: cannot write poses.txt\\n" );
		return;
	}
	f->Write( line.c_str(), line.Length() );
	fileSystem->CloseFile( f );
	common->Printf( "marked: %s", line.c_str() );
}

void R_ScreenShot_f( const idCmdArgs &args ) {"""),
("""	cmdSystem->AddCommand( "screenshot", R_ScreenShot_f, CMD_FL_RENDERER, "takes a screenshot" );""",
"""	cmdSystem->AddCommand( "screenshot", R_ScreenShot_f, CMD_FL_RENDERER, "takes a screenshot" );
	cmdSystem->AddCommand( "markPose", R_MarkPose_f, CMD_FL_RENDERER, "appends map, eye position, yaw and pitch of the current view to poses.txt in the save path (for tools\\\\pose-census.ps1)" );"""),
])
