/*
 * Copyright 2024 The Android Open Source Project
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
package androidx.media3.decoder.av1;

import static com.google.common.truth.Truth.assertThat;

import androidx.media3.common.C;
import androidx.media3.test.utils.DefaultRenderersFactoryAsserts;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import java.lang.reflect.Field;
import org.junit.Test;
import org.junit.runner.RunWith;

/** Unit test for {@link DefaultRenderersFactoryTest} with {@link Libdav1dVideoRenderer}. */
@RunWith(AndroidJUnit4.class)
public final class DefaultRenderersFactoryTest {

  @Test
  public void createRenderers_instantiatesDav1dRenderer() {
    DefaultRenderersFactoryAsserts.assertExtensionRendererCreated(
        Libdav1dVideoRenderer.class, C.TRACK_TYPE_VIDEO);
  }

  @Test
  public void defaultConstructor_usesPlaybackFriendlyDav1dTuning() throws Exception {
    Libdav1dVideoRenderer renderer =
        new Libdav1dVideoRenderer(
            /* allowedJoiningTimeMs= */ 0,
            /* eventHandler= */ null,
            /* eventListener= */ null,
            /* maxDroppedFramesToNotify= */ 0);

    Field threadsField = Libdav1dVideoRenderer.class.getDeclaredField("threads");
    threadsField.setAccessible(true);
    Field maxFrameDelayField = Libdav1dVideoRenderer.class.getDeclaredField("maxFrameDelay");
    maxFrameDelayField.setAccessible(true);

    assertThat((int) threadsField.get(renderer))
        .isEqualTo(Libdav1dVideoRenderer.THREAD_COUNT_EXPERIMENTAL);
    assertThat((int) maxFrameDelayField.get(renderer)).isEqualTo(1);
  }
}
