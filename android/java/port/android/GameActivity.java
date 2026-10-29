// The game's activity: NativeActivity, which loads libtorchlight.so and runs
// its android_main(), with two changes. The phone's Back (on a phone with
// gesture navigation, a swipe in from either side, easy to make while playing
// with the touch pad) sends the game to the background, as Home does, instead
// of finishing the activity and so ending the run. The game is left where it
// was; the engine waits in the background for its window to come back.
//
// And the system keyboard, for the names on New Game: the engine's thread
// opens it (startTextEntry) and reads its answer (textEntryState,
// textEntryText) through JNI, as the console's XamShowKeyboardUI is opened and
// waited on.
package port.android;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.os.Build;
import android.os.Bundle;
import android.text.InputFilter;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;

import java.nio.charset.StandardCharsets;

public final class GameActivity extends NativeActivity {
    /** The keyboard's answer: 0 while it is open, 1 accepted, 2 cancelled. */
    private int entryState = 2;
    private String entryText = "";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // From Android 13 Back arrives as a callback (the predictive back
        // gesture); onBackPressed() below is the older phones' path.
        if (Build.VERSION.SDK_INT >= 33) {
            OnBackInvokedCallback toBackground = () -> moveTaskToBack(true);
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                    OnBackInvokedDispatcher.PRIORITY_DEFAULT, toBackground);
        }
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        moveTaskToBack(true);
    }

    /**
     * Open the keyboard: a dialog with the title, the guide line, a line of
     * text holding `initial` and at most `maxChars` characters. Called from the
     * engine's thread; the answer is read with textEntryState().
     */
    public void startTextEntry(String title, String guide, String initial, int maxChars) {
        synchronized (this) {
            entryState = 0;
            entryText = initial;
        }
        runOnUiThread(() -> {
            EditText edit = new EditText(this);
            edit.setSingleLine(true);
            edit.setFilters(new InputFilter[] {new InputFilter.LengthFilter(maxChars)});
            edit.setText(initial);
            edit.selectAll();
            edit.setImeOptions(EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_EXTRACT_UI);
            AlertDialog dialog = new AlertDialog.Builder(
                            this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                    .setTitle(title)
                    .setMessage(guide)
                    .setView(edit)
                    .setPositiveButton(android.R.string.ok,
                            (d, which) -> answer(1, edit.getText().toString()))
                    .setNegativeButton(android.R.string.cancel, (d, which) -> answer(2, null))
                    .setOnCancelListener(d -> answer(2, null))
                    .create();
            edit.setOnEditorActionListener((view, action, event) -> {
                if (action != EditorInfo.IME_ACTION_DONE) return false;
                answer(1, edit.getText().toString());
                dialog.dismiss();
                return true;
            });
            dialog.getWindow().setSoftInputMode(
                    WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
            dialog.show();
            edit.requestFocus();
        });
    }

    private synchronized void answer(int state, String text) {
        if (entryState != 0) return;
        entryState = state;
        if (text != null) entryText = text;
    }

    /** 0 while the keyboard is open, 1 when accepted, 2 when cancelled. */
    public synchronized int textEntryState() {
        return entryState;
    }

    /** What was typed, as UTF-8. */
    public synchronized byte[] textEntryText() {
        return entryText.getBytes(StandardCharsets.UTF_8);
    }
}
