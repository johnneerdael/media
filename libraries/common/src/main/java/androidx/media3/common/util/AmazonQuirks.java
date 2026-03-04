/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package androidx.media3.common.util;

import android.content.ContentResolver;
import android.os.Build;
import android.provider.Settings.Global;

/** Fire OS quirk detection and opt-in toggles used by the Nuvio Media3 fork. */
@UnstableApi
public final class AmazonQuirks {

  private static final String FIRETV_GEN1_DEVICE_MODEL = "AFTB";
  private static final String FIRETV_STICK_DEVICE_MODEL = "AFTM";
  private static final String AMAZON = "Amazon";
  private static final String KINDLE_TABLET_DEVICE_MODEL = "KF";
  private static final String FIRE_PHONE_DEVICE_MODEL = "SD";
  private static final String FORCE_EXTERNAL_SURROUND_SOUND_KEY =
      "use_external_surround_sound_flag";
  private static final int AUDIO_HARDWARE_LATENCY_FOR_TABLETS_US = 90_000;

  private static volatile boolean experimentalFireOsAudioQuirksEnabled;
  private static volatile boolean skipProfileLevelCheck;

  private AmazonQuirks() {}

  public static void setExperimentalFireOsAudioQuirksEnabled(boolean enabled) {
    experimentalFireOsAudioQuirksEnabled = enabled;
  }

  public static boolean isExperimentalFireOsAudioQuirksEnabled() {
    return experimentalFireOsAudioQuirksEnabled;
  }

  public static boolean isAmazonDevice() {
    return AMAZON.equalsIgnoreCase(Build.MANUFACTURER);
  }

  public static boolean shouldApplyAudioQuirks() {
    return experimentalFireOsAudioQuirksEnabled && isAmazonDevice();
  }

  public static boolean isFireTvGen1Family() {
    if (!isAmazonDevice()) {
      return false;
    }
    String model = Build.MODEL != null ? Build.MODEL : "";
    return model.equalsIgnoreCase(FIRETV_GEN1_DEVICE_MODEL)
        || model.equalsIgnoreCase(FIRETV_STICK_DEVICE_MODEL);
  }

  public static boolean isDolbyPassthroughQuirkEnabled() {
    return shouldApplyAudioQuirks() && isFireTvGen1Family();
  }

  public static boolean useDefaultPassthroughDecoder() {
    return !shouldApplyAudioQuirks() || !isFireTvGen1Family();
  }

  public static boolean shouldForceLimitedFireTvDtsCoreFallback() {
    if (!shouldApplyAudioQuirks()) {
      return false;
    }
    String model = Build.MODEL != null ? Build.MODEL : "";
    return model.startsWith("AFT")
        && !"AFTGAZL".equals(model)
        && !model.startsWith("AFTB")
        && !model.startsWith("AFTA");
  }

  public static boolean shouldUseSurroundSoundFlag(ContentResolver resolver) {
    if (!shouldApplyAudioQuirks() || Util.SDK_INT < 17) {
      return false;
    }
    return Global.getInt(resolver, FORCE_EXTERNAL_SURROUND_SOUND_KEY, 0) == 1;
  }

  public static boolean isLatencyQuirkEnabled() {
    if (!shouldApplyAudioQuirks() || Util.SDK_INT > 19 || !isAmazonDevice()) {
      return false;
    }
    String model = Build.MODEL != null ? Build.MODEL : "";
    return model.startsWith(KINDLE_TABLET_DEVICE_MODEL) || model.startsWith(FIRE_PHONE_DEVICE_MODEL);
  }

  public static int getAudioHardwareLatencyUs() {
    return AUDIO_HARDWARE_LATENCY_FOR_TABLETS_US;
  }

  public static void skipProfileLevelCheck(boolean skip) {
    skipProfileLevelCheck = skip;
  }

  public static boolean shouldSkipProfileLevelCheck() {
    return skipProfileLevelCheck;
  }
}
