// The Android build's first screen: before the engine
// starts, the folder that holds the player's own copy of the game.
//
// The game's files are never in the APK. The player keeps `pak.zip` and
// `music/` in a folder of their choosing; this asks for it once with the
// system's folder picker, remembers it, checks it on every start, and says
// plainly what is missing. The engine (GameActivity, libtorchlight.so) then
// reads it by path, which is why it needs "All files access": a folder chosen
// through the picker is otherwise a content URI, and the engine opens files.
//
// It also unpacks what the APK does carry, which is part of the program and
// not of the game, into the app's own files: OGRE's shader library and the
// port's own interface pack (`ui_pack/`, what a PC pak is drawn with when no
// `console_ui/` is beside it).
//
// Last, before the game starts, its settings: on sliders, the resolution it
// renders at, as a share of the screen's (the system scales the smaller frame
// up to the screen, its proportions kept), which the engine cannot change once
// running and reads from `render_scale.txt`, a percentage, and the frame limit,
// a line of the game folder's `config.txt` (`fps_limit`), which the game reads
// and keeps too. The frame-rate overlay is a mod, switched on the mods page.
//
// Under them, a "Mod menu" switch: `config.txt`'s `mods_menu`. On, the game opens on its own
// mods page (the engine's, as on the Switch and the desktop), where each mod in the game
// folder's `mods/` is switched; off, it skips that page and starts without any mods.
package port.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.graphics.Rect;
import android.net.Uri;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;

public final class Launcher extends Activity {
    private static final int PICK_FOLDER = 1;
    private static final String PREFS = "launcher";
    private static final String KEY_FOLDER = "game_data";
    private static final String KEY_NO_MUSIC = "play_without_music";
    private static final String KEY_SCALE = "render_scale";
    /** The resolutions offered, in percent of the screen's width and height: the engine's range. */
    private static final int MIN_SCALE = 25;
    private static final int SCALE_STEP = 5;
    /** The frame limits offered; 0, at both ends of the slider, is none. */
    private static final int[] LIMITS = {0, 15, 30, 45, 60, 75, 90, 105, 120, 0};
    /** The engine's own default on Android, when config.txt does not say. */
    private static final int DEFAULT_LIMIT = 60;

    /**
     * The game is running in this process (in the background, or this activity was started over
     * it): it is brought back rather than started again. The process ends with the game.
     */
    private static boolean gameRunning;

    /** A dialog or a system screen is up; onResume() waits for it. */
    private boolean waiting;
    /** The game has been started; nothing more to do here. */
    private boolean started;

    @Override
    protected void onResume() {
        super.onResume();
        if (gameRunning) {
            started = true;
            startActivity(new Intent(this, GameActivity.class)
                    .addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT));
            finish();
            return;
        }
        if (!waiting) next();
    }

    private SharedPreferences prefs() {
        return getSharedPreferences(PREFS, MODE_PRIVATE);
    }

    /** One step further towards the game, or the question that stands in the way. */
    private void next() {
        if (started) return;
        if (!Environment.isExternalStorageManager()) {
            askForStorage();
            return;
        }
        String folder = prefs().getString(KEY_FOLDER, null);
        if (folder == null) {
            askForFolder("Choose the folder with your game files",
                    "This program does not include the game. Choose the folder that holds your "
                            + "own copy of it:\n\n  pak.zip   (the Xbox 360 release's)\n  music/\n\n"
                            + "Copy them from your game to a folder on this phone first, "
                            + "for example Download/Torchlight.");
            return;
        }
        File dir = new File(folder);
        if (!new File(dir, "pak.zip").isFile() && !new File(dir, "Pak.zip").isFile()) {
            askForFolder("pak.zip is missing",
                    "There is no pak.zip in\n\n  " + folder + "\n\nChoose the folder that holds "
                            + "pak.zip (the Xbox 360 release's) and music/.");
            return;
        }
        if (!new File(dir, "music").isDirectory() && !folder.equals(prefs().getString(KEY_NO_MUSIC, ""))) {
            askAboutMusic(folder);
            return;
        }
        askForSettings(folder);
    }

    /** The screen's size in landscape, which is how the game is played. */
    private int[] screenSize() {
        Rect bounds = getWindowManager().getMaximumWindowMetrics().getBounds();
        int w = bounds.width();
        int h = bounds.height();
        return new int[] {Math.max(w, h), Math.min(w, h)};
    }

    /**
     * What the engine renders at for a percentage: the screen's size times 1/factor, the factor
     * written and read back as the engine gives it to OGRE, so the figure shown is the frame's.
     */
    private static int scaled(int size, int percent) {
        if (percent >= 100) return size;
        float factor = (float) Double.parseDouble(String.format(Locale.ROOT, "%.6f", 100.0 / percent));
        return (int) (size * (1.0f / factor));
    }

    private int dp(int value) {
        return (int) (value * getResources().getDisplayMetrics().density);
    }

    private static String scaleText(int[] screen, int percent) {
        return "Resolution: " + percent + "%   " + scaled(screen[0], percent) + " x "
                + scaled(screen[1], percent) + (percent == 100 ? "   (the screen's own)" : "");
    }

    private static String limitText(int fps) {
        return "Frame limit: " + (fps == 0 ? "none" : fps + " frames a second");
    }

    /** A slider under its label, the label kept saying what the slider is at. */
    private interface Label {
        String of(int position);
    }

    private SeekBar slider(LinearLayout box, int max, int position, Label label, Runnable moved) {
        TextView text = new TextView(this);
        text.setText(label.of(position));
        text.setPadding(0, dp(12), 0, dp(4));
        SeekBar bar = new SeekBar(this);
        bar.setMax(max);
        bar.setProgress(position);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar b, int progress, boolean fromUser) {
                text.setText(label.of(progress));
                if (fromUser) moved.run();
            }

            @Override
            public void onStartTrackingTouch(SeekBar b) {}

            @Override
            public void onStopTrackingTouch(SeekBar b) {}
        });
        box.addView(text);
        box.addView(bar);
        return bar;
    }

    /** An on/off line under the sliders. */
    private Switch toggle(LinearLayout box, String text, boolean on) {
        Switch toggle = new Switch(this);
        toggle.setText(text);
        toggle.setChecked(on);
        toggle.setPadding(0, dp(12), 0, 0);
        box.addView(toggle);
        return toggle;
    }

    /** The last question: the settings, the mods page's switch among them. */
    private void askForSettings(String folder) {
        waiting = true;
        int[] screen = screenSize();
        int chosen = Math.max(MIN_SCALE, Math.min(100, prefs().getInt(KEY_SCALE, 100)));
        int limit = readConfig(folder, "fps_limit", DEFAULT_LIMIT);
        int limitAt = 0;
        for (int i = 1; i < LIMITS.length - 1; ++i) {
            if (Math.abs(LIMITS[i] - limit) < Math.abs(LIMITS[limitAt] - limit)) limitAt = i;
        }
        if (limit == 0) limitAt = LIMITS.length - 1;

        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24), dp(4), dp(24), 0);
        final boolean[] limitMoved = {false};
        SeekBar scale = slider(box, (100 - MIN_SCALE) / SCALE_STEP, (chosen - MIN_SCALE) / SCALE_STEP,
                p -> scaleText(screen, MIN_SCALE + p * SCALE_STEP), () -> {});
        SeekBar frames = slider(box, LIMITS.length - 1, limitAt, p -> limitText(LIMITS[p]),
                () -> limitMoved[0] = true);
        boolean modMenu = readConfig(folder, "mods_menu", 1) != 0;
        Switch mods = toggle(box, "Mod menu", modMenu);

        new AlertDialog.Builder(this)
                .setTitle("Torchlight")
                .setView(box)
                .setCancelable(false)
                .setPositiveButton("Play", (d, w) -> {
                    prefs().edit().putInt(KEY_SCALE, MIN_SCALE + scale.getProgress() * SCALE_STEP).apply();
                    if (limitMoved[0]) {
                        writeConfig(folder, "fps_limit", LIMITS[frames.getProgress()], DEFAULT_LIMIT,
                                "the most frames a second the game draws; 0 for no limit.");
                    }
                    if (mods.isChecked() != modMenu) {
                        writeConfig(folder, "mods_menu", mods.isChecked() ? 1 : 0, 1,
                                "1 shows the mods page before the game; 0 skips it and starts without any mods.");
                    }
                    waiting = false;
                    start(folder);
                })
                .setNeutralButton("Game folder", (d, w) ->
                        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_FOLDER))
                .setNegativeButton("Quit", (d, w) -> finish())
                .show();
    }

    /**
     * A number line of `config.txt` in the game folder, as the engine reads it (`fps_limit`, or
     * `0`/`1` for a switch): `fallback`, the engine's own default, when it says nothing.
     */
    private static int readConfig(String folder, String wanted, int fallback) {
        try {
            for (String line : read(new File(folder, "config.txt")).split("\r?\n")) {
                String[] kv = line.split("#", 2)[0].split("=", 2);
                if (kv.length < 2 || !kv[0].trim().equalsIgnoreCase(wanted)) continue;
                int value = Integer.parseInt(kv[1].trim());
                if (value >= 0 && value <= 1000) return value;
            }
        } catch (IOException | NumberFormatException e) {
            // No file yet, or a line the engine would pass over too.
        }
        return fallback;
    }

    /**
     * One line of `config.txt` set, the rest of the file kept; a key it lacks is added under its
     * comment. With no file yet, and the engine's default asked for, none is made: the engine
     * writes its own on the first start.
     */
    private static void writeConfig(String folder, String wanted, int value, int fallback,
            String comment) {
        File file = new File(folder, "config.txt");
        String text;
        try {
            text = read(file);
        } catch (IOException e) {
            if (value == fallback) return;
            text = "";
        }
        StringBuilder out = new StringBuilder();
        boolean set = false;
        String[] lines = text.isEmpty() ? new String[0] : text.split("\r?\n", -1);
        // The piece after the last line break is not a line.
        int count = lines.length > 0 && lines[lines.length - 1].isEmpty() ? lines.length - 1 : lines.length;
        for (int i = 0; i < count; ++i) {
            String line = lines[i];
            String key = line.split("#", 2)[0].split("=", 2)[0].trim();
            if (!set && key.equalsIgnoreCase(wanted) && line.contains("=")) {
                out.append(wanted).append('=').append(value).append('\n');
                set = true;
            } else {
                out.append(line).append('\n');
            }
        }
        if (!set) {
            out.append("# ").append(wanted).append(": ").append(comment).append('\n');
            out.append(wanted).append('=').append(value).append('\n');
        }
        try (OutputStream o = new FileOutputStream(file)) {
            o.write(out.toString().getBytes("UTF-8"));
        } catch (IOException e) {
            // The engine then keeps what the file said.
        }
    }

    private void askForStorage() {
        waiting = true;
        new AlertDialog.Builder(this)
                .setTitle("Allow access to your game files")
                .setMessage("The game reads pak.zip and music/ from a folder you choose. Android "
                        + "lets a program read a folder that way only with \"All files access\".\n\n"
                        + "On the next screen, switch it on for this program, then come back.")
                .setCancelable(false)
                .setPositiveButton("Open settings", (d, w) -> {
                    Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                            Uri.parse("package:" + getPackageName()));
                    waiting = false;  // checked again when the settings screen returns
                    startActivity(intent);
                })
                .setNegativeButton("Quit", (d, w) -> finish())
                .show();
    }

    private void askForFolder(String title, String message) {
        waiting = true;
        new AlertDialog.Builder(this)
                .setTitle(title)
                .setMessage(message)
                .setCancelable(false)
                .setPositiveButton("Choose folder", (d, w) ->
                        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_FOLDER))
                .setNegativeButton("Quit", (d, w) -> finish())
                .show();
    }

    private void askAboutMusic(String folder) {
        waiting = true;
        new AlertDialog.Builder(this)
                .setTitle("music/ is missing")
                .setMessage("There is no music/ folder in\n\n  " + folder + "\n\nThe level themes are "
                        + "not inside pak.zip. Copy the music/ folder from your game beside it, or "
                        + "play without music.")
                .setCancelable(false)
                .setPositiveButton("Play without music", (d, w) -> {
                    prefs().edit().putString(KEY_NO_MUSIC, folder).apply();
                    waiting = false;
                    next();
                })
                .setNeutralButton("Choose folder", (d, w) ->
                        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_FOLDER))
                .setNegativeButton("Quit", (d, w) -> finish())
                .show();
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_FOLDER) return;
        // onResume(), which follows, takes the next step with the folder chosen.
        waiting = false;
        if (result == RESULT_OK && data != null && data.getData() != null) {
            String path = pathOf(data.getData());
            if (path != null) prefs().edit().putString(KEY_FOLDER, path).apply();
        }
    }

    /**
     * A folder the picker returned, as a path the engine can open: `primary:Download/x` is the
     * phone's own storage, `XXXX-XXXX:x` an SD card's. Null for anything else.
     */
    private static String pathOf(Uri tree) {
        String id = DocumentsContract.getTreeDocumentId(tree);
        int colon = id.indexOf(':');
        if (colon < 0) return null;
        String volume = id.substring(0, colon);
        String rest = id.substring(colon + 1);
        String root = volume.equals("primary")
                ? Environment.getExternalStorageDirectory().getPath()
                : "/storage/" + volume;
        return rest.isEmpty() ? root : root + "/" + rest;
    }

    private void start(String folder) {
        try {
            unpackShaderLibrary();
        } catch (IOException e) {
            waiting = true;
            new AlertDialog.Builder(this)
                    .setTitle("The program could not unpack itself")
                    .setMessage("Its shader library could not be written to the app's own storage ("
                            + e.getMessage() + "). Free some space and try again.")
                    .setPositiveButton("Quit", (d, w) -> finish())
                    .show();
            return;
        }
        File app = getExternalFilesDir(null);
        try (OutputStream out = new FileOutputStream(new File(app, "game_data.txt"))) {
            out.write((folder + "\n").getBytes("UTF-8"));
        } catch (IOException e) {
            // The engine then reads the app's own folder, and says what it does not find there.
        }
        try (OutputStream out = new FileOutputStream(new File(app, "render_scale.txt"))) {
            out.write((prefs().getInt(KEY_SCALE, 100) + "\n").getBytes("UTF-8"));
        } catch (IOException e) {
            // The engine then renders at the screen's own resolution.
        }
        started = true;
        gameRunning = true;
        startActivity(new Intent(this, GameActivity.class));
        finish();
    }

    /**
     * OGRE's shader library (`RTShaderLib/`, `Main/` inside it) and the interface pack
     * (`ui_pack/`), from the APK's assets into the app's files, once for each install or update
     * of the program.
     */
    private void unpackShaderLibrary() throws IOException {
        long installed;
        try {
            installed = getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime;
        } catch (PackageManager.NameNotFoundException e) {
            installed = 0;
        }
        File stamp = new File(getFilesDir(), "RTShaderLib.stamp");
        String want = Long.toString(installed);
        if (stamp.isFile() && want.equals(read(stamp))) return;
        copyAssets(getAssets(), "RTShaderLib", new File(getFilesDir(), "RTShaderLib"));
        // Replaced whole: a file of an older pack (or one spelt in another case) would stay.
        File pack = new File(getFilesDir(), "ui_pack");
        delete(pack);
        copyAssets(getAssets(), "ui_pack", pack);
        try (OutputStream out = new FileOutputStream(stamp)) {
            out.write(want.getBytes("UTF-8"));
        }
    }

    private static void delete(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) delete(child);
        }
        file.delete();
    }

    private static void copyAssets(AssetManager assets, String from, File to) throws IOException {
        String[] names = assets.list(from);
        if (names == null || names.length == 0) {
            try (InputStream in = assets.open(from); OutputStream out = new FileOutputStream(to)) {
                byte[] buffer = new byte[65536];
                for (int n; (n = in.read(buffer)) > 0; ) out.write(buffer, 0, n);
            }
            return;
        }
        if (!to.isDirectory() && !to.mkdirs()) throw new IOException("cannot make " + to);
        for (String name : names) copyAssets(assets, from + "/" + name, new File(to, name));
    }

    private static String read(File file) throws IOException {
        try (InputStream in = new java.io.FileInputStream(file)) {
            java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
            byte[] buffer = new byte[256];
            for (int n; (n = in.read(buffer)) > 0; ) bytes.write(buffer, 0, n);
            return new String(bytes.toByteArray(), "UTF-8").trim();
        }
    }
}
