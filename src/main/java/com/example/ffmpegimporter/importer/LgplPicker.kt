package com.example.ffmpegimporter.importer

import android.Manifest
import android.app.Activity
import android.net.Uri
import android.provider.OpenableColumns
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import java.io.FileOutputStream
import java.io.InputStream

@Composable
fun LgplNoticeAndSoPickerDialog(onDismiss: () -> Unit, onSoSelected: (File) -> Unit) {
    val context = LocalContext.current
    var showFilePicker by remember { mutableStateOf(false) }

    val pickSoFileLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.GetContent(),
        onResult = { uri: Uri? ->
            uri?.let {
                val fileName = getFileName(context as Activity, uri)
                if (fileName.endsWith(".so")) {
                    val outputFile = File(context.filesDir, fileName)
                    context.contentResolver.openInputStream(uri)?.use { inputStream ->
                        saveFile(inputStream, outputFile)
                        onSoSelected(outputFile)
                    }
                }
            }
        }
    )

    AlertDialog(
        onDismissRequest = onDismiss,
        title = {
            Text("FFmpeg LGPL Notice")
        },
        text = {
            Column(modifier = Modifier.fillMaxWidth()) {
                Text(
                    "This app uses FFmpeg libraries licensed under the LGPL v2.1. " +
                            "You can provide your own compatible .so files by selecting them."
                )
                Spacer(modifier = Modifier.height(16.dp))
                Button(onClick = { showFilePicker = true }, modifier = Modifier.align(Alignment.CenterHorizontally)) {
                    Text("Select .so File")
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss) {
                Text("Close")
            }
        }
    )

    if (showFilePicker) {
        LaunchedEffect(Unit) {
            pickSoFileLauncher.launch("application/octet-stream")
            showFilePicker = false
        }
    }
}

private fun getFileName(activity: Activity, uri: Uri): String {
    var result: String? = null
    if (uri.scheme == "content") {
        val cursor = activity.contentResolver.query(uri, null, null, null, null)
        cursor?.use {
            if (it.moveToFirst()) {
                val res = it.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (res >= 0) {
                    result = it.getString(res)
                }
            }
        }
    }
    if (result == null) {
        result = uri.path
        val cut = result?.lastIndexOf('/') ?: -1
        if (cut != -1) {
            result = result?.substring(cut + 1)
        }
    }
    return result ?: "unknown.so"
}

private fun saveFile(inputStream: InputStream, outputFile: File) {
    FileOutputStream(outputFile).use { outputStream ->
        val buffer = ByteArray(4096)
        var bytesRead: Int
        while (inputStream.read(buffer).also { bytesRead = it } != -1) {
            outputStream.write(buffer, 0, bytesRead)
        }
    }
}
