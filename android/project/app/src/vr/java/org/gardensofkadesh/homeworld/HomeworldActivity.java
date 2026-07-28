package org.gardensofkadesh.homeworld;

import android.content.res.AssetFileDescriptor;
import android.content.res.AssetManager;
import android.os.Bundle;
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

    private boolean fullGame = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Before super: it is super.onCreate that calls getLibraries() and
        // loads the .so, so the decision has to already be made by then.
        prepareGameData();
        super.onCreate(savedInstanceState);
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
        File big = findIgnoringCase(files, FULL_BIG);

        fullGame = (big != null);
        if (fullGame) {
            Log.i(TAG, "found " + big.getName() + ": full campaign");
            for (String expected : FULL_FILES) {
                File actual = findIgnoringCase(files, expected);

                if (actual != null) {
                    renameToExpectedCase(actual, expected);
                }
            }
            removeBundledDemoAssets(files);
        } else {
            Log.i(TAG, "no " + FULL_BIG + ": demo campaign");
            unpackDemoAssets(files);
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

    /** Length of a bundled asset, or -1 if this APK does not carry it. */
    private long bundledLength(String name) {
        try (AssetFileDescriptor fd = getAssets().openFd(name)) {
            return fd.getLength();
        } catch (IOException e) {
            return -1;
        }
    }
}
