package com.a32;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.lang.reflect.Method;
import java.security.MessageDigest;
import java.util.UUID;

/**
 * Per-install identifiers for games that build a "device id" from the IMEI and the Wi-Fi MAC. Android 10+ hides both
 * (the IMEI is not readable, the MAC is always 02:00:00:00:00:00), so every device would compute the same id.
 * The id is derived from Settings.Secure.ANDROID_ID, which Android keeps per device (and per signing key) across restarts
 * and reinstalls, so a device always gets the same id and different devices get different ones. Only when ANDROID_ID is
 * unusable is a random 128-bit value generated and kept in the app's files directory. The MAC and IMEI the game sees
 * are derived from the id. Uses only java.* so it can be compiled with plain javac (see tools/install_ids/build.sh).
 */
public final class InstallIds {
    private static String cached;

    private InstallIds() {}

    private static String packageName() throws Exception {
        FileInputStream in = new FileInputStream("/proc/self/cmdline");
        try {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[256];
            int n = in.read(buf);
            for (int i = 0; i < n && buf[i] != 0; i++) out.write(buf[i]);
            String name = out.toString("UTF-8");
            int colon = name.indexOf(':');  // a secondary process shares the main process' id
            return colon < 0 ? name : name.substring(0, colon);
        } finally {
            in.close();
        }
    }

    private static String androidId() {
        try {
            Object app = Class.forName("android.app.ActivityThread").getMethod("currentApplication").invoke(null);
            Object resolver = app.getClass().getMethod("getContentResolver").invoke(app);
            Class<?> secure = Class.forName("android.provider.Settings$Secure");
            Method get = secure.getMethod("getString", Class.forName("android.content.ContentResolver"), String.class);
            String value = (String) get.invoke(null, resolver, "android_id");
            // empty, or the value a broken Android 2.2 build gave to every device
            return value == null || value.length() == 0 || value.equals("9774d56d682e549c") ? null : value;
        } catch (Throwable t) {
            return null;
        }
    }

    private static String md5Hex(String text) throws Exception {
        byte[] d = MessageDigest.getInstance("MD5").digest(text.getBytes("UTF-8"));
        StringBuilder sb = new StringBuilder();
        for (byte b : d) sb.append(String.format("%02x", b & 0xff));
        return sb.toString();
    }

    /** 32 hex characters: stable per device (ANDROID_ID), or per install when ANDROID_ID is not available. */
    public static synchronized String id() {
        if (cached != null) return cached;
        try {
            String android = androidId();
            if (android != null) {
                cached = md5Hex(android + ":" + packageName());
                return cached;
            }
        } catch (Throwable ignored) {
            // fall through to the saved random id
        }
        String fresh = UUID.randomUUID().toString().replace("-", "");
        try {
            File dir = new File("/data/data/" + packageName() + "/files");
            File file = new File(dir, ".a32_install_id");
            if (file.exists() && file.length() == 32) {
                byte[] buf = new byte[32];
                FileInputStream in = new FileInputStream(file);
                try {
                    if (in.read(buf) == 32) {
                        cached = new String(buf, "US-ASCII");
                        return cached;
                    }
                } finally {
                    in.close();
                }
            }
            dir.mkdirs();
            FileOutputStream out = new FileOutputStream(file);
            try {
                out.write(fresh.getBytes("US-ASCII"));
            } finally {
                out.close();
            }
        } catch (Throwable ignored) {
            // read-only data directory: the id is then only stable for this run
        }
        cached = fresh;
        return cached;
    }

    private static boolean usable(String mac) {
        return mac != null && mac.length() > 0 && !mac.equals("02:00:00:00:00:00") && !mac.equals("00:00:00:00:00:00");
    }

    /** The real MAC when Android still gives one out, otherwise a locally administered one derived from the id. */
    public static String mac(String real) {
        if (usable(real)) return real;
        String id = id();
        StringBuilder sb = new StringBuilder("02");  // locally administered, unicast
        for (int i = 1; i < 6; i++) sb.append(':').append(id, i * 2, i * 2 + 2);
        return sb.toString();
    }

    /** The real IMEI when readable, otherwise 15 digits derived from the id. */
    public static String imei(String real) {
        if (real != null && real.length() > 0) return real;
        String id = id();
        long a = Long.parseLong(id.substring(8, 16), 16);
        long b = Long.parseLong(id.substring(16, 24), 16);
        long c = Long.parseLong(id.substring(24, 32), 16);
        String digits = Long.toString(((a << 32 | b) ^ (c * 0x9E3779B97F4A7C15L)) & Long.MAX_VALUE);
        while (digits.length() < 15) digits = "0" + digits;
        return digits.substring(0, 15);
    }
}
