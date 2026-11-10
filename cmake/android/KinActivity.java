package org.kin.runtime;

import org.libsdl.app.SDLActivity;

// The activity of every kin Android app: SDL's, with SDL linked into the
// game's own libmain.so (kin links SDL statically) instead of a libSDL3.so.
public class KinActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }
}
