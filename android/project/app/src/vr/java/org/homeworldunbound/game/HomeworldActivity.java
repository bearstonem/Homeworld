package org.homeworldunbound.game;

import android.app.AlarmManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.AssetFileDescriptor;
import android.content.res.AssetManager;
import android.net.Uri;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

import org.libsdl.app.SDLActivity;

/**
 * One APK, both campaigns.
 *
 * HW_GAME_DEMO is a compile-time decision in the engine and reaches a long way
 * in: the .big file it opens, the music and speech filenames, and the mission
 * sequence itself are all baked at build time, the last of them as a literal
 * array. No single native library can present both, so the VR flavour ships
 * two - libmain.so built with -Ddemo=false and libmainDemo.so with
 * -Ddemo=true - and this picks between them before SDL loads either.
 *
 * The rule is the presence of Homeworld.big in the app's external files
 * directory, which is where the player drops the data from a copy of the game
 * they own. Absent it, the bundled 1.05 demo assets are unpacked there on
 * first run so the app is playable straight off SideQuest.
 */
public class HomeworldActivity extends SDLActivity {

    private static final String TAG = "SDL/APP";

    /**
     * {asset name, name on disk}. The two differ for the voice file: the demo
     * assets ship "DL_demo.vce" and the demo binary opens "DL_Demo.vce".
     * Android's emulated storage is case-insensitive in practice, but writing
     * the name the engine asks for removes the question.
     */
    private static final String[][] DEMO_ASSETS = {
        { "HomeworldDL.big", "HomeworldDL.big" },
        { "DL_Music.wxd",    "DL_Music.wxd"    },
        { "DL_demo.vce",     "DL_Demo.vce"     },
        { "Update.big",      "Update.big"      },
    };

    /**
     * Shipped by the port rather than by Relic, and wanted whichever campaign
     * is being played, so it is unpacked on every run instead of only when
     * the demo assets are. The multiplayer loading screen falls back to it
     * because no .big carries ScreenShots/ShotList.script.
     */
    private static final String LOADING_IMAGE = "loading.jpg";

    /** What the full build opens, and so what "the player brought their own" means. */
    private static final String FULL_BIG = "Homeworld.big";

    /**
     * The names the full build opens, which are not the case the Remastered
     * Collection ships: it has "homeworld.big" and "HW_Comp.vce" against the
     * engine's "Homeworld.big" and "HW_comp.vce". Anything dragged in by hand
     * is renamed to match, so nothing downstream has to rely on the storage
     * layer being case-insensitive.
     */
    private static final String[] FULL_FILES = {
        FULL_BIG, "HW_Music.wxd", "HW_comp.vce",
    };

    /**
     * Where to look for game data the player put somewhere they can actually
     * reach, relative to the top of shared storage. Searched in this order,
     * and one level below each of them, which covers both "dropped in
     * Downloads" and "dropped in Downloads/Homeworld".
     *
     * This exists because Android 11 closed Android/data to file managers.
     * The app's own directory is the only place it could read from before,
     * and it is precisely the one place a headset with no PC attached cannot
     * write to - so the full campaign was unreachable standalone.
     */
    private static final String[] DATA_SEARCH_ROOTS = {
        Environment.DIRECTORY_DOWNLOADS,
        Environment.DIRECTORY_DOCUMENTS,
        null,                                   //the top of shared storage
    };

    private static final String PREFS = "homeworld";
    private static final String PREF_ASKED_FOR_FILES = "askedForAllFiles";

    private boolean fullGame = false;

    /**
     * The directory the engine will read game data from: the app's own by
     * default, or wherever the player left their files. Settings and saves do
     * not follow it - see getArguments.
     */
    private File dataDir;

    /**
     * Held for the lifetime of the activity so LAN games can be discovered.
     *
     * The Wi-Fi stack drops packets that are not addressed to this device,
     * which is a sensible power saving default and fatal to a protocol whose
     * whole discovery mechanism is a UDP broadcast. Without this the game
     * hosts and joins perfectly well by typed address and simply never sees
     * anybody advertise, which is a confusing way for it to fail.
     */
    private WifiManager.MulticastLock multicastLock;

    /**
     * Which engine this process actually loaded, or null before the first
     * load. Static because it outlives the activity: Android reuses a warm
     * process, and System.loadLibrary cannot be undone.
     */
    private static Boolean loadedFullGame = null;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Before super: it is super.onCreate that calls getLibraries() and
        // loads the .so, so the decision has to already be made by then.
        prepareGameData();

        // The demo/full choice is compiled into the engine, so it is fixed for
        // the life of a process the moment a library is loaded. The data that
        // decides it is not: a player runs the demo, copies their game into
        // Downloads, and comes back. Android warm-starts the existing process,
        // prepareGameData now says full campaign, and the already-loaded
        // libmainDemo.so goes looking for HomeworldDL.big in Downloads and
        // dies with "Unable to find required .big file" - leaving the headset
        // in the compositor with nothing to draw. Launching a second time gets
        // a fresh process and works, which is what made it look like a
        // first-launch quirk rather than a crash.
        //
        // There is no way to unload a library, so the only correct answer is
        // not to run in this process. Exit before super.onCreate touches the
        // wrong one; the next launch starts clean and picks the right engine.
        if (loadedFullGame != null && loadedFullGame.booleanValue() != fullGame) {
            Log.i(TAG, "campaign changed (was " + (loadedFullGame ? "full" : "demo")
                     + ", now " + (fullGame ? "full" : "demo")
                     + ") - restarting so the right engine is loaded");
            scheduleRelaunch();
            finishAffinity();
            System.exit(0);
            return;
        }
        loadedFullGame = Boolean.valueOf(fullGame);

        acquireMulticastLock();
        super.onCreate(savedInstanceState);
    }

    /**
     * Come back by ourselves after exiting for a campaign change.
     *
     * Without this the restart is correct and looks exactly like the crash it
     * replaced: the app vanishes before drawing anything, the headset holds
     * its loading environment, and the player has to launch a second time with
     * no idea why. Not crashing is worth little if what you see is unchanged.
     *
     * An alarm rather than a direct start, because the intent has to survive
     * this process exiting - which is the entire point of it.
     */
    private void scheduleRelaunch() {
        try {
            Intent intent =
                getPackageManager().getLaunchIntentForPackage(getPackageName());

            if (intent == null) {
                Log.w(TAG, "no launch intent; the next start has to be manual");
                return;
            }
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                          | Intent.FLAG_ACTIVITY_CLEAR_TASK);

            PendingIntent pending = PendingIntent.getActivity(
                this, 0, intent,
                PendingIntent.FLAG_CANCEL_CURRENT | PendingIntent.FLAG_IMMUTABLE);
            AlarmManager alarms =
                (AlarmManager)getSystemService(Context.ALARM_SERVICE);

            if (alarms == null) {
                Log.w(TAG, "no AlarmManager; the next start has to be manual");
                return;
            }
            /* Long enough that this process is gone before the new one asks
               for a library, short enough not to read as a hang. */
            /* setExact, not set: set has been inexact since API 19 and is
               batched with whatever else the system has pending, which turned
               a 400ms request into nearly seven seconds - long enough to read
               as the hang this is supposed to remove. */
            alarms.setExact(AlarmManager.RTC,
                            System.currentTimeMillis() + RELAUNCH_DELAY_MS, pending);
            Log.i(TAG, "relaunch scheduled in " + RELAUNCH_DELAY_MS + " ms");
        } catch (Exception e) {
            // Never let the restart path be the thing that breaks a launch:
            // failing to schedule costs one manual press, throwing costs more.
            Log.w(TAG, "could not schedule relaunch: " + e);
        }
    }

    private static final long RELAUNCH_DELAY_MS = 400;

    @Override
    protected void onDestroy() {
        if (multicastLock != null && multicastLock.isHeld()) {
            multicastLock.release();
        }
        multicastLock = null;
        super.onDestroy();
    }

    private void acquireMulticastLock() {
        try {
            WifiManager wifi =
                (WifiManager)getApplicationContext().getSystemService(Context.WIFI_SERVICE);

            if (wifi == null) {
                Log.w(TAG, "no WifiManager; LAN game discovery may not work");
                return;
            }
            multicastLock = wifi.createMulticastLock("homeworld-lan");
            multicastLock.setReferenceCounted(false);
            multicastLock.acquire();
            Log.i(TAG, "multicast lock acquired for LAN discovery");
        } catch (Exception e) {
            // Never worth failing to start the game over. Discovery degrades,
            // joining by address still works.
            Log.w(TAG, "could not acquire multicast lock", e);
        }
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {
            "SDL2",
            fullGame ? "main" : "mainDemo",
        };
    }

    private void prepareGameData() {
        File files = getExternalFilesDir(null);

        if (files == null) {
            Log.e(TAG, "no external files directory; falling back to the demo build");
            return;
        }
        dataDir = files;

        File big = findIgnoringCase(files, FULL_BIG);

        if (big != null) {
            // Brought over by install.py or adb, into the app's own directory.
            // Renaming is safe and worth doing here: we own these.
            Log.i(TAG, "found " + big.getName() + ": full campaign");
            fullGame = true;
            for (String expected : FULL_FILES) {
                File actual = findIgnoringCase(files, expected);

                if (actual != null) {
                    renameToExpectedCase(actual, expected);
                }
            }
            removeBundledDemoAssets(files);
        } else {
            File elsewhere = findDataInSharedStorage();

            if (elsewhere != null) {
                // Read where the player left it. Nothing is renamed and
                // nothing is copied: these are the player's files, sitting in
                // the player's folder, and a 600MB copy onto a headset is a
                // poor way to start. The engine coped with the spelling as
                // shipped once bigOpenAllBigFiles stopped opening the
                // uncorrected name.
                Log.i(TAG, "found " + FULL_BIG + " in " + elsewhere
                           + ": full campaign, reading in place");
                fullGame = true;
                dataDir = elsewhere;
                setDataPathForEngine(elsewhere);
            } else {
                Log.i(TAG, "no " + FULL_BIG + " anywhere reachable: demo campaign");
                unpackDemoAssets(files);
                /* Deliberately not asking for All files access at all.

                   The request opens a system Settings panel. From onCreate
                   that hung the app outright: the panel takes focus, SDL will
                   not start the game thread without focus, and focus never
                   returned to something that had never drawn a frame. Moving
                   it to onResume fixed the hang - the game runs and draws -
                   but the panel still takes focus and Quest does not reliably
                   give it back, so the game was left drawing and unable to be
                   interacted with. A prompt that costs the player the use of
                   the game is worse than no prompt.

                   The demo does not need the permission. Someone who owns the
                   game gets it from install.py, which grants it over adb, or
                   by hand in the headset's own settings - and the release
                   notes say so. */
                Log.i(TAG, "demo campaign; All files access not requested "
                         + "(grant it in headset settings, or run install.py, "
                         + "to play a copy of the game you own)");
            }
        }

        // Into whichever directory the engine will actually search, since
        // that is the only one it will look in for this.
        unpackAsset(dataDir, LOADING_IMAGE);
    }

    /**
     * Settings, saves and screenshots stay in the app's own directory even
     * when the data is read from somewhere else. It is guaranteed writable,
     * it is where any existing install already keeps them, and it means
     * pointing the game at a read-only folder still works.
     *
     * Without this the engine would put them beside the data: with no $HOME
     * on Android, fileUserSettingsPath defaults to fileHomeworldDataPath.
     */
    @Override
    protected String[] getArguments() {
        File files = getExternalFilesDir(null);

        if (files == null) {
            return super.getArguments();
        }
        return new String[] { "/settingspath", files.getAbsolutePath() };
    }

    /**
     * HW_Data is read by utyStartup and wins over everything else, including
     * a stale absolute path remembered in the config. Set before super.onCreate
     * so it is in place well before the engine starts.
     */
    private void setDataPathForEngine(File dir) {
        try {
            Os.setenv("HW_Data", dir.getAbsolutePath(), true);
        } catch (ErrnoException e) {
            // Then the engine looks in its own directory and finds the demo.
            // Playable, just not what was asked for.
            Log.e(TAG, "could not set HW_Data to " + dir, e);
            fullGame = false;
            dataDir = getExternalFilesDir(null);
        }
    }

    /**
     * Look for the player's own copy somewhere they can reach without a PC.
     * Needs All files access; without it the directories below list as empty
     * rather than failing, so the check is worth making explicitly.
     */
    private File findDataInSharedStorage() {
        if (!hasAllFilesAccess()) {
            return null;
        }
        for (String root : DATA_SEARCH_ROOTS) {
            File dir = (root == null)
                ? Environment.getExternalStorageDirectory()
                : Environment.getExternalStoragePublicDirectory(root);

            if (dir == null || !dir.isDirectory()) {
                continue;
            }
            if (findIgnoringCase(dir, FULL_BIG) != null) {
                return dir;
            }
            File[] entries = dir.listFiles();
            if (entries == null) {
                continue;
            }
            for (File sub : entries) {
                if (sub.isDirectory() && findIgnoringCase(sub, FULL_BIG) != null) {
                    return sub;
                }
            }
        }
        return null;
    }

    private boolean hasAllFilesAccess() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            return true;                        //no scoped storage to get past
        }
        return Environment.isExternalStorageManager();
    }

    /**
     * Ask once, ever, and only when there is no full game to play anyway.
     *
     * Someone happy with the demo should not be sent to a system settings
     * screen every launch, and someone who installed through install.py never
     * sees this at all because their data is already in place. Once asked, the
     * answer stands: they can turn it on later from the app's settings, which
     * is what the README says to do.
     */
    private void requestAllFilesAccessOnce() {
        if (hasAllFilesAccess()) {
            return;
        }
        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);

        if (prefs.getBoolean(PREF_ASKED_FOR_FILES, false)) {
            Log.i(TAG, "no file access and already asked once; staying on the demo");
            return;
        }
        prefs.edit().putBoolean(PREF_ASKED_FOR_FILES, true).apply();
        try {
            Log.i(TAG, "asking for All files access so the full game can be found");
            startActivity(new Intent(
                Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                Uri.parse("package:" + getPackageName())));
        } catch (Exception e) {
            // Some runtimes do not present that screen. The demo still runs,
            // and the permission can be granted from the app's settings.
            Log.w(TAG, "could not open the All files access screen", e);
        }
    }

    /**
     * The Remastered Collection ships the file as "homeworld.big" and the
     * engine opens "Homeworld.big". Emulated storage is case-insensitive in
     * practice, so the engine finds it either way - but java.io.File is not
     * guaranteed to be, and someone dragging the file in through SideQuest's
     * file manager without renaming it would otherwise get the demo for ever
     * with nothing on screen to say why. Cheap to just look properly.
     */
    private File findIgnoringCase(File dir, String name) {
        File exact = new File(dir, name);

        if (exact.isFile()) {
            return exact;
        }
        File[] entries = dir.listFiles();
        if (entries != null) {
            for (File f : entries) {
                if (f.isFile() && f.getName().equalsIgnoreCase(name)) {
                    return f;
                }
            }
        }
        return null;
    }

    /**
     * Put it under the name the engine asks for, so the case-insensitivity of
     * the storage layer is never load-bearing. A failed rename is not fatal -
     * the file is found either way, and the engine opens it through the C
     * library, which goes through the same case-insensitive layer.
     */
    private void renameToExpectedCase(File found, String expected) {
        if (found.getName().equals(expected)) {
            return;
        }
        File target = new File(found.getParentFile(), expected);

        Log.i(TAG, "renaming " + found.getName() + " to " + expected + " ("
                   + (found.renameTo(target) ? "ok" : "failed, carrying on") + ")");
    }

    /**
     * Update.big sits FIRST in the engine's bigFilePrecedence, so a demo copy
     * left behind silently overrides full-game content with demo content -
     * the classic way this port gets misdiagnosed as broken. Clearing the demo
     * files once the real ones arrive is the whole point of doing this from
     * Java rather than leaving it to the player.
     *
     * Only files still byte-identical in length to the ones this APK carries
     * are removed. Update.big is a name the retail 1.05 patch uses too, so a
     * player who brought their own must keep it.
     */
    private void removeBundledDemoAssets(File files) {
        for (String[] asset : DEMO_ASSETS) {
            File onDisk = new File(files, asset[1]);

            if (!onDisk.isFile()) {
                continue;
            }
            long bundled = bundledLength(asset[0]);
            if (bundled >= 0 && bundled == onDisk.length()) {
                Log.i(TAG, "removing bundled demo asset " + asset[1]
                           + " (" + (onDisk.delete() ? "ok" : "FAILED") + ")");
            } else {
                Log.i(TAG, "keeping " + asset[1] + ": not the copy this APK ships"
                           + " (" + onDisk.length() + " vs " + bundled + ")");
            }
        }
    }

    /**
     * First run only, and on the main thread on purpose: it has to finish
     * before the engine starts looking for its data, there is nothing useful
     * to show while it runs, and the assets are stored uncompressed (see
     * androidResources.noCompress in build.gradle) so this is a straight copy
     * of about 65 MB rather than an inflate.
     */
    private void unpackDemoAssets(File files) {
        AssetManager assets = getAssets();

        if (!files.isDirectory() && !files.mkdirs()) {
            Log.e(TAG, "cannot create " + files);
            return;
        }
        for (String[] asset : DEMO_ASSETS) {
            File target = new File(files, asset[1]);
            long bundled = bundledLength(asset[0]);

            if (target.isFile() && target.length() == bundled) {
                continue;                           //already unpacked and complete
            }
            long started = System.currentTimeMillis();
            try (InputStream in = assets.open(asset[0]);
                 OutputStream out = new FileOutputStream(target)) {
                byte[] buffer = new byte[1 << 16];
                int read;

                while ((read = in.read(buffer)) > 0) {
                    out.write(buffer, 0, read);
                }
            } catch (IOException e) {
                Log.e(TAG, "unpacking " + asset[1] + " failed", e);
                target.delete();                    //a half-written .big is worse than none
                continue;
            }
            Log.i(TAG, "unpacked " + asset[1] + " (" + target.length() + " bytes in "
                       + (System.currentTimeMillis() - started) + " ms)");
        }
    }

    /** Copy one bundled asset out if it is not already there and complete. */
    private void unpackAsset(File files, String name) {
        File target = new File(files, name);
        long bundled = bundledLength(name);

        if (bundled < 0 || (target.isFile() && target.length() == bundled)) {
            return;
        }
        if (!files.isDirectory() && !files.mkdirs()) {
            return;
        }
        try (InputStream in = getAssets().open(name);
             OutputStream out = new FileOutputStream(target)) {
            byte[] buffer = new byte[1 << 16];
            int read;

            while ((read = in.read(buffer)) > 0) {
                out.write(buffer, 0, read);
            }
            Log.i(TAG, "unpacked " + name + " (" + target.length() + " bytes)");
        } catch (IOException e) {
            Log.e(TAG, "unpacking " + name + " failed", e);
            target.delete();
        }
    }

    /** Length of a bundled asset, or -1 if this APK does not carry it. */
    private long bundledLength(String name) {
        try (AssetFileDescriptor fd = getAssets().openFd(name)) {
            return fd.getLength();
        } catch (IOException e) {
            return -1;
        }
    }
}
