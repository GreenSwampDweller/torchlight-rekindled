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
//
// And a two-screen handheld's other display: when the engine asks
// (setSecondScreen) a window of the game's is put on it, whose surface is
// handed to the engine (secondSurface) to draw the minimap on.
package port.android;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.app.Presentation;
import android.hardware.display.DisplayManager;
import android.os.Build;
import android.os.Bundle;
import android.text.InputFilter;
import android.view.Display;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
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

    /** The second display's window (setSecondScreen), all on the UI thread. */
    private boolean secondWanted;
    private boolean foreground;
    private Presentation second;
    private DisplayManager.DisplayListener displayListener;

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
        hideSystemBars();
    }

    /**
     * The whole screen: the status and navigation bars hidden until a swipe
     * from the edge shows them for a moment (immersive mode). A desktop mode
     * (Samsung DeX) keeps its taskbar and the window's title bar over an app
     * that does not ask for this, and runs it in a window.
     */
    @SuppressWarnings("deprecation")  // setDecorFitsSystemWindows: the layout under the bars before Android 15
    private void hideSystemBars() {
        Window window = getWindow();
        window.setDecorFitsSystemWindows(false);
        WindowInsetsController controller = window.getInsetsController();
        if (controller == null) return;
        controller.setSystemBarsBehavior(
                WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        controller.hide(WindowInsets.Type.systemBars());
    }

    /**
     * The engine's, registered by it (second_screen.cpp): the surface on the
     * second display when it is made or changes size, null when it is taken
     * away. Returns once the engine has stopped drawing into one taken away.
     */
    private static native void secondSurface(Surface surface);

    /**
     * Show the engine's window on the second display, or take it down. Called
     * from the engine's thread; nothing happens on a device with one screen.
     */
    public void setSecondScreen(boolean on) {
        runOnUiThread(() -> {
            secondWanted = on;
            if (on && displayListener == null) {
                // A display plugged in or taken away while the game runs.
                displayListener = new DisplayManager.DisplayListener() {
                    @Override public void onDisplayAdded(int id) { updateSecondScreen(); }
                    @Override public void onDisplayRemoved(int id) { updateSecondScreen(); }
                    @Override public void onDisplayChanged(int id) {}
                };
                getSystemService(DisplayManager.class).registerDisplayListener(displayListener, null);
            }
            updateSecondScreen();
        });
    }

    /**
     * The display for the second window: one the system offers for
     * presentations (a two-screen handheld's other screen, a television), else
     * any other display than the game's own. Null with one screen.
     */
    private Display secondDisplay() {
        DisplayManager displays = getSystemService(DisplayManager.class);
        int own = getDisplay() != null ? getDisplay().getDisplayId() : Display.DEFAULT_DISPLAY;
        for (Display display : displays.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION)) {
            if (display.getDisplayId() != own) return display;
        }
        for (Display display : displays.getDisplays()) {
            if (display.getDisplayId() != own && (display.getFlags() & Display.FLAG_PRIVATE) == 0) {
                return display;
            }
        }
        return null;
    }

    /**
     * The second window as it should be now: up while the engine wants it, the
     * game is in the foreground and there is a display for it. A Presentation:
     * a dialog of this activity shown on another display, here one black view
     * whose surface the engine draws into. It never takes the focus, so the
     * controller's buttons stay with the game's own window.
     */
    private void updateSecondScreen() {
        Display display = secondWanted && foreground ? secondDisplay() : null;
        if (second != null && (display == null
                || second.getDisplay().getDisplayId() != display.getDisplayId()
                || !second.isShowing())) {
            second.dismiss();  // surfaceDestroyed() tells the engine
            second = null;
        }
        if (display == null || second != null) return;
        Presentation presentation = new Presentation(this, display);
        presentation.setCancelable(false);
        Window window = presentation.getWindow();
        window.addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        SurfaceView view = new SurfaceView(presentation.getContext());
        view.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder holder) {}
            @Override public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {
                tellEngine(holder.getSurface());
            }
            @Override public void surfaceDestroyed(SurfaceHolder holder) {
                tellEngine(null);
            }
        });
        presentation.setContentView(view);
        try {
            presentation.show();
        } catch (WindowManager.InvalidDisplayException gone) {
            return;  // the display went between the choice and the show
        }
        second = presentation;
        // The whole of that screen too, as hideSystemBars() does this one.
        window.setDecorFitsSystemWindows(false);
        WindowInsetsController controller = window.getInsetsController();
        if (controller != null) {
            controller.setSystemBarsBehavior(
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            controller.hide(WindowInsets.Type.systemBars());
        }
    }

    private static void tellEngine(Surface surface) {
        try {
            secondSurface(surface);
        } catch (UnsatisfiedLinkError notRegistered) {
            // An engine that never registered the method never asked for the window.
        }
    }

    @Override
    protected void onStart() {
        super.onStart();
        foreground = true;
        updateSecondScreen();
    }

    @Override
    protected void onStop() {
        foreground = false;
        updateSecondScreen();
        super.onStop();
    }

    @Override
    protected void onDestroy() {
        if (displayListener != null) {
            getSystemService(DisplayManager.class).unregisterDisplayListener(displayListener);
            displayListener = null;
        }
        super.onDestroy();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        // The bars come back with a dialog (the keyboard's) or a switch away;
        // they are hidden again once the game has the window back.
        if (hasFocus) hideSystemBars();
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
