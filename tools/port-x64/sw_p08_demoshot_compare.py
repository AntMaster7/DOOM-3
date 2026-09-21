"""com_demoShotCompare: the listed demo frames go through swCompareShot (both back ends, same frame)
instead of the screenshot path."""
import sys
sys.path.insert(0, 'C:/Source/DOOM-3/tools/port-x64')
from px import patch

patch('framework/Session.cpp', [
("""static idCVar com_demoShotTag(""", """static idCVar com_demoShotCompare( "com_demoShotCompare", "0", CVAR_SYSTEM | CVAR_BOOL, "com_demoShotFrames: write each frame as rendered by GL AND by the software renderer (swCompareShot), print the difference" );
static idCVar com_demoShotTag("""),
("""			console->ClearNotifyLines();
			renderSystem->TakeScreenshot(""", """			console->ClearNotifyLines();
			if ( com_demoShotCompare.GetBool() ) {
				// the redraw of this demo frame goes through both back ends
				common->Printf( "demo frame %i: ", numDemoFrames );
				cmdSystem->BufferCommandText( CMD_EXEC_NOW, va( "swCompareShot \\"demoshots/%s_cmp_f%05i\\"\\n", demoName.c_str(), numDemoFrames ) );
				UpdateScreen();
				break;
			}
			renderSystem->TakeScreenshot("""),
])
