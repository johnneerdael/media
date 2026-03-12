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
import java.util.concurrent.CopyOnWriteArraySet;

/** Fire OS quirk detection and opt-in toggles used by the Nuvio Media3 fork. */
@UnstableApi
public final class AmazonQuirks {
  /** Listener notified when Kodi/Fire OS audio quirk settings change. */
  public interface Listener {
    void onAmazonQuirkSettingsChanged();
  }


  private static final String FIRETV_GEN1_DEVICE_MODEL = "AFTB";
  private static final String FIRETV_STICK_DEVICE_MODEL = "AFTM";
  private static final String AMAZON = "Amazon";
  private static final String KINDLE_TABLET_DEVICE_MODEL = "KF";
  private static final String FIRE_PHONE_DEVICE_MODEL = "SD";
  private static final String FORCE_EXTERNAL_SURROUND_SOUND_KEY =
      "use_external_surround_sound_flag";
  private static final int AUDIO_HARDWARE_LATENCY_FOR_TABLETS_US = 90_000;

  private static volatile boolean experimentalFireOsAudioQuirksEnabled;
  private static volatile boolean limitedFireTvDtsCoreFallbackEnabled;
  private static volatile boolean skipProfileLevelCheck;
  private static volatile boolean fireOsIecVerboseLoggingEnabled;
  private static volatile boolean fireOsIecSuperviseAudioDelayEnabled;
  private static volatile boolean iecPackerAc3PassthroughEnabled = true;
  private static volatile boolean iecPackerEac3PassthroughEnabled = true;
  private static volatile boolean iecPackerDtsPassthroughEnabled = true;
  private static volatile boolean iecPackerTruehdPassthroughEnabled = true;
  private static volatile boolean iecPackerDtshdPassthroughEnabled = true;
  private static volatile boolean iecPackerDtshdCoreFallbackEnabled = true;
  private static volatile boolean iecPackerAc3TranscodeEnabled;
  private static volatile int iecPackerAudioConfig;
  private static volatile int iecPackerMaxPcmChannelLayout = 10;
  private static volatile String iecPackerAudioDevice = "";
  private static volatile String iecPackerPassthroughDevice = "";
  private static final CopyOnWriteArraySet<Listener> listeners = new CopyOnWriteArraySet<>();

  private AmazonQuirks() {}

  public static void addListener(Listener listener) {
    listeners.add(listener);
  }

  public static void removeListener(Listener listener) {
    listeners.remove(listener);
  }

  public static void setExperimentalFireOsAudioQuirksEnabled(boolean enabled) {
    experimentalFireOsAudioQuirksEnabled = enabled;
    notifyListeners();
  }

  public static boolean isExperimentalFireOsAudioQuirksEnabled() {
    return experimentalFireOsAudioQuirksEnabled;
  }

  public static void setExperimentalFireOsIecPassthroughEnabled(boolean enabled) {
    experimentalFireOsAudioQuirksEnabled = enabled;
    notifyListeners();
  }

  public static void setFireOsCompatibilityFallbackEnabled(boolean enabled) {
    limitedFireTvDtsCoreFallbackEnabled = enabled;
    notifyListeners();
  }

  public static void setFireOsIecVerboseLoggingEnabled(boolean enabled) {
    fireOsIecVerboseLoggingEnabled = enabled;
    notifyListeners();
  }

  public static boolean isFireOsIecVerboseLoggingEnabled() {
    return fireOsIecVerboseLoggingEnabled;
  }

  public static void setFireOsIecSuperviseAudioDelayEnabled(boolean enabled) {
    fireOsIecSuperviseAudioDelayEnabled = enabled;
    notifyListeners();
  }

  public static boolean isFireOsIecSuperviseAudioDelayEnabled() {
    return fireOsIecSuperviseAudioDelayEnabled;
  }

  public static void setIecPackerAc3PassthroughEnabled(boolean enabled) {
    iecPackerAc3PassthroughEnabled = enabled;
    notifyListeners();
  }

  public static boolean isIecPackerAc3PassthroughEnabled() {
    return iecPackerAc3PassthroughEnabled;
  }

  public static void setIecPackerEac3PassthroughEnabled(boolean enabled) {
    iecPackerEac3PassthroughEnabled = enabled;
    notifyListeners();
  }

  public static boolean isIecPackerEac3PassthroughEnabled() {
    return iecPackerEac3PassthroughEnabled;
  }

  public static void setIecPackerDtsPassthroughEnabled(boolean enabled) {
    iecPackerDtsPassthroughEnabled = enabled;
    notifyListeners();
  }

  public static boolean isIecPackerDtsPassthroughEnabled() {
    return iecPackerDtsPassthroughEnabled;
  }

  public static void setIecPackerTruehdPassthroughEnabled(boolean enabled) {
    iecPackerTruehdPassthroughEnabled = enabled;
    notifyListeners();
  }

  public static boolean isIecPackerTruehdPassthroughEnabled() {
    return iecPackerTruehdPassthroughEnabled;
  }

  public static void setIecPackerDtshdPassthroughEnabled(boolean enabled) {
    iecPackerDtshdPassthroughEnabled = enabled;
    notifyListeners();
  }

  public static boolean isIecPackerDtshdPassthroughEnabled() {
    return iecPackerDtshdPassthroughEnabled;
  }

  public static void setIecPackerDtshdCoreFallbackEnabled(boolean enabled) {
    iecPackerDtshdCoreFallbackEnabled = enabled;
    notifyListeners();
  }

  public static boolean isIecPackerDtshdCoreFallbackEnabled() {
    return iecPackerDtshdCoreFallbackEnabled;
  }

  public static void setIecPackerAc3TranscodeEnabled(boolean enabled) {
    iecPackerAc3TranscodeEnabled = enabled;
    notifyListeners();
  }

  public static boolean isIecPackerAc3TranscodeEnabled() {
    return iecPackerAc3TranscodeEnabled;
  }

  public static void setIecPackerAudioConfig(int config) {
    iecPackerAudioConfig = config;
    notifyListeners();
  }

  public static int getIecPackerAudioConfig() {
    return iecPackerAudioConfig;
  }

  public static void setIecPackerMaxPcmChannelLayout(int channelLayout) {
    iecPackerMaxPcmChannelLayout = channelLayout;
    notifyListeners();
  }

  public static int getIecPackerMaxPcmChannelLayout() {
    return iecPackerMaxPcmChannelLayout;
  }

  public static void setIecPackerAudioDevice(String device) {
    iecPackerAudioDevice = device != null ? device : "";
    notifyListeners();
  }

  public static String getIecPackerAudioDevice() {
    return iecPackerAudioDevice;
  }

  public static void setIecPackerPassthroughDevice(String device) {
    iecPackerPassthroughDevice = device != null ? device : "";
    notifyListeners();
  }

  public static String getIecPackerPassthroughDevice() {
    return iecPackerPassthroughDevice;
  }

  public static boolean isFireOsCompatibilityFallbackEnabled() {
    return limitedFireTvDtsCoreFallbackEnabled;
  }

  public static void setLimitedFireTvDtsCoreFallbackEnabled(boolean enabled) {
    setFireOsCompatibilityFallbackEnabled(enabled);
  }

  public static boolean isLimitedFireTvDtsCoreFallbackEnabled() {
    return isFireOsCompatibilityFallbackEnabled();
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
    if (!limitedFireTvDtsCoreFallbackEnabled || !isAmazonDevice()) {
      return false;
    }
    String model = Build.MODEL != null ? Build.MODEL : "";
    // Treat all Fire TV "AFT*" models as DTS-core-only for passthrough stability.
    return model.startsWith("AFT");
  }

  public static boolean shouldAttemptExperimentalFireOsIecPassthrough() {
    if (!shouldApplyAudioQuirks()) {
      return false;
    }
    String model = Build.MODEL != null ? Build.MODEL : "";
    return model.startsWith("AFT");
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

  private static void notifyListeners() {
    for (Listener listener : listeners) {
      listener.onAmazonQuirkSettingsChanged();
    }
  }
}
