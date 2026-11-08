package com.audiobookbridge.soundscape.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.AssistChip
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.audiobookbridge.soundscape.engine.SoundscapeController
import com.audiobookbridge.soundscape.engine.SoundscapeMode
import com.audiobookbridge.soundscape.engine.SoundscapeUiState
import com.audiobookbridge.soundscape.model.Scene
import kotlin.math.roundToInt

/**
 * Drop-in for the player screen: the scene chip plus its picker sheet.
 *
 * ```
 * SoundscapePlayerControls(controller)   // e.g. in the row under the seek bar
 * ```
 */
@Composable
fun SoundscapePlayerControls(controller: SoundscapeController, modifier: Modifier = Modifier) {
    val state by controller.uiState.collectAsState()
    var open by rememberSaveable { mutableStateOf(false) }
    SoundscapeChip(state, onClick = { open = true }, modifier = modifier)
    if (open) {
        ScenePickerSheet(
            state = state,
            onDismiss = { open = false },
            onPickScene = controller::pickScene,
            onPickAuto = controller::pickAuto,
            onEnabledChange = controller::setEnabled,
            onMasterChange = controller::setMaster,
        )
    }
}

/** Small chip showing the current scene, e.g. "Rain", "Auto · Tavern" or "Ambience off". */
@Composable
fun SoundscapeChip(state: SoundscapeUiState, onClick: () -> Unit, modifier: Modifier = Modifier) {
    val e = state.engine
    val text = when {
        !e.enabled -> "Ambience off"
        e.mode == SoundscapeMode.AUTO -> "Auto · ${e.scene.label}"
        e.scene == Scene.NONE -> "Ambience"
        else -> e.scene.label
    }
    AssistChip(onClick = onClick, label = { Text("♪ $text") }, modifier = modifier)
}

@OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)
@Composable
fun ScenePickerSheet(
    state: SoundscapeUiState,
    onDismiss: () -> Unit,
    onPickScene: (Scene) -> Unit,
    onPickAuto: () -> Unit,
    onEnabledChange: (Boolean) -> Unit,
    onMasterChange: (Float) -> Unit,
) {
    val e = state.engine
    val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    // Slider moves locally while dragging and saves once on release.
    var level by remember(e.master) { mutableFloatStateOf(e.master) }

    ModalBottomSheet(onDismissRequest = onDismiss, sheetState = sheetState) {
        Column(
            Modifier
                .fillMaxWidth()
                .padding(horizontal = 20.dp)
                .navigationBarsPadding(),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Ambience", style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f))
                Switch(checked = e.enabled, onCheckedChange = onEnabledChange)
            }
            Spacer(Modifier.height(8.dp))
            Text(
                "Level: ${(level * 100).roundToInt()}% of narration",
                style = MaterialTheme.typography.bodySmall,
            )
            Slider(
                value = level,
                onValueChange = { level = it },
                onValueChangeFinished = { onMasterChange(level) },
                valueRange = 0f..0.6f,
                enabled = e.enabled,
            )
            Spacer(Modifier.height(8.dp))
            FlowRow(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalArrangement = Arrangement.spacedBy(4.dp),
            ) {
                if (state.timelineLoaded && state.autoScenesSetting) {
                    FilterChip(
                        selected = e.enabled && e.mode == SoundscapeMode.AUTO,
                        onClick = onPickAuto,
                        label = { Text("Auto (follows the book)") },
                    )
                }
                for (scene in Scene.entries) {
                    FilterChip(
                        selected = e.mode == SoundscapeMode.MANUAL && state.manualScene == scene && e.enabled,
                        onClick = { onPickScene(scene) },
                        label = { Text(scene.label) },
                    )
                }
            }
            Spacer(Modifier.height(24.dp))
        }
    }
}
