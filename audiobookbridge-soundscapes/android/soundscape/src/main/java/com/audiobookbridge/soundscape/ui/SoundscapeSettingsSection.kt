package com.audiobookbridge.soundscape.ui

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.audiobookbridge.soundscape.engine.SoundscapeController
import kotlinx.coroutines.launch

/**
 * Section for the app's settings screen: PC URL, "auto scenes" toggle and a
 * "Test connection" button.
 */
@Composable
fun SoundscapeSettingsSection(controller: SoundscapeController, modifier: Modifier = Modifier) {
    val state by controller.uiState.collectAsState()
    val scope = rememberCoroutineScope()
    var url by remember(state.baseUrl) { mutableStateOf(state.baseUrl) }
    var testing by remember { mutableStateOf(false) }
    var result by remember { mutableStateOf<String?>(null) }

    Column(modifier.fillMaxWidth().padding(16.dp)) {
        Text("Soundscapes", style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(8.dp))
        OutlinedTextField(
            value = url,
            onValueChange = { url = it; result = null },
            label = { Text("PC URL") },
            placeholder = { Text("http://my-pc.tailnet.ts.net:8642") },
            singleLine = true,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri, imeAction = ImeAction.Done),
            modifier = Modifier.fillMaxWidth(),
        )
        Spacer(Modifier.height(8.dp))
        Row(verticalAlignment = Alignment.CenterVertically) {
            Button(
                enabled = !testing,
                onClick = {
                    controller.setBaseUrl(url)
                    testing = true
                    result = null
                    scope.launch {
                        val r = controller.testConnection(url)
                        result = r.fold({ it }, { it.message ?: "Failed" })
                        testing = false
                    }
                },
            ) { Text("Save & test connection") }
            if (testing) {
                Spacer(Modifier.width(12.dp))
                CircularProgressIndicator(Modifier.width(20.dp).height(20.dp), strokeWidth = 2.dp)
            }
        }
        result?.let {
            Spacer(Modifier.height(4.dp))
            Text(it, style = MaterialTheme.typography.bodySmall)
        }
        Spacer(Modifier.height(12.dp))
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text("Auto scenes")
                Text(
                    "Change the ambience with the story, using timelines made on the PC.",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
            Switch(checked = state.autoScenesSetting, onCheckedChange = controller::setAutoScenes)
        }
    }
}
