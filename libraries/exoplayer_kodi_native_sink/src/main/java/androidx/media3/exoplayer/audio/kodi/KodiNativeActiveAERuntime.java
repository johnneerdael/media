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

import android.app.Activity;
import android.app.ActivityManager;
import android.app.Application;
import android.content.ComponentCallbacks2;
import android.content.Context;
import android.content.res.Configuration;
import android.media.AudioDeviceInfo;
import android.os.Bundle;
import androidx.annotation.Nullable;
import androidx.media3.common.AudioAttributes;
import androidx.media3.common.util.AmazonQuirks;
import androidx.media3.common.util.UnstableApi;
import androidx.media3.exoplayer.audio.AudioCapabilities;
import androidx.media3.exoplayer.audio.AudioCapabilitiesReceiver;
import java.util.concurrent.CopyOnWriteArraySet;

/** Minimal ActiveAE-style runtime owner for settings and app focus propagation. */
@UnstableApi
public final class KodiNativeActiveAERuntime implements AutoCloseable, AmazonQuirks.Listener {

  /** Listener for runtime state changes that affect sink control or playback decisions. */
  public interface Listener {
    void onUserAudioSettingsChanged();

    void onControlSettingsChanged();

    void onAppFocusedChanged(boolean appFocused);

    void onCapabilitySnapshotChanged(KodiNativeCapabilitySnapshot capabilitySnapshot);

    void onEngineStatsChanged(KodiNativeEngineStats stats);
  }

  private final CopyOnWriteArraySet<Listener> listeners;
  private final AppFocusMonitor appFocusMonitor;
  private final AudioCapabilitiesReceiver audioCapabilitiesReceiver;
  private final Context context;
  private volatile KodiNativeUserAudioSettings userAudioSettings;
  private volatile KodiNativeAudioSink.ControlSettings controlSettings;
  private volatile boolean appFocused;
  private volatile KodiNativeCapabilitySnapshot capabilitySnapshot;
  private volatile KodiNativeEngineStats engineStats;
  private volatile AudioAttributes audioAttributes;
  @Nullable private volatile AudioDeviceInfo routedDevice;

  public static KodiNativeActiveAERuntime createDefault(Context context) {
    return new KodiNativeActiveAERuntime(
        context, KodiNativeUserAudioSettings.fromGlobals(), KodiNativeAudioSink.ControlSettings.DEFAULT);
  }

  public KodiNativeActiveAERuntime(
      Context context,
      KodiNativeUserAudioSettings userAudioSettings,
      KodiNativeAudioSink.ControlSettings controlSettings) {
    this.listeners = new CopyOnWriteArraySet<>();
    this.context = context.getApplicationContext();
    this.userAudioSettings = userAudioSettings;
    this.controlSettings = controlSettings;
    this.audioAttributes = AudioAttributes.DEFAULT;
    this.routedDevice = null;
    this.appFocused = sampleAppFocusedState(this.context);
    this.capabilitySnapshot =
        KodiNativeCapabilitySnapshot.fromSystem(this.context, audioAttributes, routedDevice);
    this.engineStats = KodiNativeEngineStats.EMPTY;
    this.appFocusMonitor = new AppFocusMonitor(this.context, this::setAppFocusedInternal);
    this.audioCapabilitiesReceiver =
        new AudioCapabilitiesReceiver(
            this.context,
            this::onAudioCapabilitiesChanged,
            audioAttributes,
            routedDevice);
    this.audioCapabilitiesReceiver.register();
    AmazonQuirks.addListener(this);
  }

  public void addListener(Listener listener) {
    listeners.add(listener);
  }

  public void removeListener(Listener listener) {
    listeners.remove(listener);
  }

  public KodiNativeUserAudioSettings getUserAudioSettings() {
    return userAudioSettings;
  }

  public KodiNativeAudioSink.ControlSettings getControlSettings() {
    return controlSettings;
  }

  public boolean isAppFocused() {
    return appFocused;
  }

  public KodiNativeCapabilitySnapshot getCapabilitySnapshot() {
    return capabilitySnapshot;
  }

  public KodiNativeEngineStats getEngineStats() {
    return engineStats;
  }

  public void setRoutingContext(
      AudioAttributes audioAttributes, @Nullable AudioDeviceInfo routedDevice) {
    this.audioAttributes = audioAttributes;
    this.routedDevice = routedDevice;
    audioCapabilitiesReceiver.setAudioAttributes(audioAttributes);
    audioCapabilitiesReceiver.setRoutedDevice(routedDevice);
    refreshCapabilitySnapshot();
  }

  public void setUserAudioSettings(KodiNativeUserAudioSettings userAudioSettings) {
    this.userAudioSettings = userAudioSettings;
    for (Listener listener : listeners) {
      listener.onUserAudioSettingsChanged();
    }
  }

  public void reloadUserAudioSettingsFromGlobals() {
    setUserAudioSettings(KodiNativeUserAudioSettings.fromGlobals());
  }

  public void setControlSettings(KodiNativeAudioSink.ControlSettings controlSettings) {
    this.controlSettings = controlSettings;
    for (Listener listener : listeners) {
      listener.onControlSettingsChanged();
    }
  }

  public void setEngineStats(KodiNativeEngineStats stats) {
    this.engineStats = stats;
    for (Listener listener : listeners) {
      listener.onEngineStatsChanged(stats);
    }
  }

  @Override
  public void close() {
    AmazonQuirks.removeListener(this);
    audioCapabilitiesReceiver.unregister();
    appFocusMonitor.release();
    listeners.clear();
  }

  @Override
  public void onAmazonQuirkSettingsChanged() {
    reloadUserAudioSettingsFromGlobals();
  }

  private void setAppFocusedInternal(boolean appFocused) {
    if (this.appFocused == appFocused) {
      return;
    }
    this.appFocused = appFocused;
    for (Listener listener : listeners) {
      listener.onAppFocusedChanged(appFocused);
    }
  }

  private void onAudioCapabilitiesChanged(AudioCapabilities audioCapabilities) {
    refreshCapabilitySnapshot();
  }

  private void refreshCapabilitySnapshot() {
    KodiNativeCapabilitySnapshot snapshot =
        KodiNativeCapabilitySnapshot.fromSystem(context, audioAttributes, routedDevice);
    if (snapshot.equals(capabilitySnapshot)) {
      return;
    }
    capabilitySnapshot = snapshot;
    for (Listener listener : listeners) {
      listener.onCapabilitySnapshotChanged(snapshot);
    }
  }

  private static boolean sampleAppFocusedState(Context context) {
    ActivityManager.RunningAppProcessInfo processInfo =
        new ActivityManager.RunningAppProcessInfo();
    ActivityManager.getMyMemoryState(processInfo);
    return processInfo.importance <= ActivityManager.RunningAppProcessInfo.IMPORTANCE_VISIBLE;
  }

  private static final class AppFocusMonitor
      implements Application.ActivityLifecycleCallbacks, ComponentCallbacks2 {
    private final Application application;
    private final FocusCallback callback;
    private int startedActivityCount;
    private boolean released;
    private boolean appFocused;

    public AppFocusMonitor(Context context, FocusCallback callback) {
      Context appContext = context.getApplicationContext();
      if (!(appContext instanceof Application)) {
        throw new IllegalArgumentException("Application context is required");
      }
      this.application = (Application) appContext;
      this.callback = callback;
      this.appFocused = sampleAppFocusedState(application);
      application.registerActivityLifecycleCallbacks(this);
      application.registerComponentCallbacks(this);
      callback.onAppFocusChanged(appFocused);
    }

    public void release() {
      if (released) {
        return;
      }
      released = true;
      application.unregisterActivityLifecycleCallbacks(this);
      application.unregisterComponentCallbacks(this);
    }

    @Override
    public void onActivityStarted(Activity activity) {
      startedActivityCount++;
      setAppFocused(true);
    }

    @Override
    public void onActivityStopped(Activity activity) {
      if (startedActivityCount > 0) {
        startedActivityCount--;
      }
      if (startedActivityCount == 0) {
        setAppFocused(sampleAppFocusedState(application));
      }
    }

    @Override
    public void onActivityResumed(Activity activity) {
      setAppFocused(true);
    }

    @Override
    public void onTrimMemory(int level) {
      if (level >= ComponentCallbacks2.TRIM_MEMORY_UI_HIDDEN) {
        setAppFocused(false);
      }
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {}

    @Override
    public void onLowMemory() {}

    @Override
    public void onActivityCreated(Activity activity, Bundle savedInstanceState) {}

    @Override
    public void onActivityPaused(Activity activity) {}

    @Override
    public void onActivitySaveInstanceState(Activity activity, Bundle outState) {}

    @Override
    public void onActivityDestroyed(Activity activity) {}

    private void setAppFocused(boolean appFocused) {
      if (released || this.appFocused == appFocused) {
        return;
      }
      this.appFocused = appFocused;
      callback.onAppFocusChanged(appFocused);
    }
  }

  private interface FocusCallback {
    void onAppFocusChanged(boolean appFocused);
  }
}
