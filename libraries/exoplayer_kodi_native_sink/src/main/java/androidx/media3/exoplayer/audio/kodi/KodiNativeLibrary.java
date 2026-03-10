/*
 * Copyright (C) 2026 Nuvio
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
package androidx.media3.exoplayer.audio.kodi;

import androidx.media3.common.MediaLibraryInfo;
import androidx.media3.common.util.LibraryLoader;
import androidx.media3.common.util.UnstableApi;

/** Loads and introspects the native Kodi-backed audio sink library. */
@UnstableApi
public final class KodiNativeLibrary {

  private static final int SMOKE_TEST_SENTINEL = 0x4B4F4449;

  static {
    MediaLibraryInfo.registerModule("media3.exoplayer.kodi.native.sink");
  }

  private static final LibraryLoader LOADER =
      new LibraryLoader("kodiNativeSinkJNI") {
        @Override
        protected void loadLibrary(String name) {
          System.loadLibrary(name);
        }
      };

  private KodiNativeLibrary() {}

  /** Returns whether the native library is available, loading it if needed. */
  public static boolean isAvailable() {
    try {
      return LOADER.isAvailable();
    } catch (RuntimeException | UnsatisfiedLinkError e) {
      return false;
    }
  }

  /** Returns whether the loaded JNI library responds with the expected smoke-test value. */
  public static boolean passesSmokeTest() {
    return isAvailable() && nGetSmokeTestValue() == SMOKE_TEST_SENTINEL;
  }

  /** Returns the raw JNI smoke-test value for diagnostics. */
  public static int getSmokeTestValue() {
    if (!isAvailable()) {
      throw new IllegalStateException("Kodi native sink library is unavailable");
    }
    return nGetSmokeTestValue();
  }

  private static native int nGetSmokeTestValue();
}
