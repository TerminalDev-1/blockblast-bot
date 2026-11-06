// Tablet-side helper for the Block Blast bot (v5.5-cpp-max). Runs as the adb shell user
// via app_process. It keeps a live mirror of the screen in memory, so the latest frame is
// ready in ~0.04 s (screencap takes ~0.37 s), and injects touches without starting the
// `input` tool for every swipe. Android only offers both of these to Java code, so this
// small part can't be C++ (the PC side is).
//
// Protocol on the local socket "bbhelper" (reach it with `adb forward`), one command per line:
//   cap X Y W H                  -> "F <w> <h> <len> <ageMs> <frame>\n" + zlib(RGB rows)
//   swipe X1 Y1 X2 Y2 MS HOLD    -> "ok\n"  finger down, moves every ~8 ms for MS, rests HOLD ms, up
//   ping                         -> "ok\n"
// Build with build-helper.ps1 (JDK + Android SDK).
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.hardware.display.VirtualDisplay;
import android.media.Image;
import android.media.ImageReader;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.MotionEvent;
import android.view.Surface;

import java.io.BufferedOutputStream;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.util.zip.Deflater;

public class BBHelper {
    static final Object lock = new Object();
    static Image latest;
    static long frames, latestAt;
    static Method inject;
    static Object inputManager;

    public static void main(String[] args) throws Exception {
        int w = Integer.parseInt(args[0]), h = Integer.parseInt(args[1]);
        setUpInput();
        HandlerThread thread = new HandlerThread("frames");
        thread.start();
        ImageReader reader = ImageReader.newInstance(w, h, PixelFormat.RGBA_8888, 3);
        reader.setOnImageAvailableListener(r -> {
            Image im;
            try { im = r.acquireLatestImage(); } catch (Exception e) { return; }
            if (im == null) return;
            synchronized (lock) {
                if (latest != null) latest.close();
                latest = im; frames++; latestAt = SystemClock.uptimeMillis();
                lock.notifyAll();
            }
        }, new Handler(thread.getLooper()));
        mirror(reader.getSurface(), w, h);

        LocalServerSocket server = new LocalServerSocket("bbhelper");
        System.out.println("ready"); System.out.flush();
        while (true) {  // one client at a time
            LocalSocket client = server.accept();
            try {
                serve(new BufferedReader(new InputStreamReader(client.getInputStream())),
                      new BufferedOutputStream(client.getOutputStream(), 1 << 16));
            } catch (Exception e) { System.err.println("client: " + e); }
            try { client.close(); } catch (Exception e) { }
        }
    }

    static void serve(BufferedReader in, OutputStream out) throws Exception {
        Deflater deflater = new Deflater(1);
        byte[] rgb = new byte[0], packed = new byte[0], row = new byte[0];
        String line;
        while ((line = in.readLine()) != null) {
            String[] a = line.trim().split(" ");
            try {
                if (a[0].equals("cap")) {
                    int x = Integer.parseInt(a[1]), y = Integer.parseInt(a[2]);
                    int cw = Integer.parseInt(a[3]), ch = Integer.parseInt(a[4]);
                    long frame, age;
                    synchronized (lock) {
                        long end = SystemClock.uptimeMillis() + 2000;
                        while (latest == null && SystemClock.uptimeMillis() < end) lock.wait(50);
                        if (latest == null) throw new IllegalStateException("no frame yet");
                        Image.Plane p = latest.getPlanes()[0];
                        ByteBuffer buf = p.getBuffer();
                        int stride = p.getRowStride(), px = p.getPixelStride();
                        if (rgb.length < cw * ch * 3) rgb = new byte[cw * ch * 3];
                        if (row.length < cw * px) row = new byte[cw * px];
                        int k = 0;
                        for (int yy = 0; yy < ch; yy++) {
                            buf.position((y + yy) * stride + x * px);
                            buf.get(row, 0, cw * px);
                            for (int i = 0; i < cw * px; i += px) {
                                rgb[k++] = row[i]; rgb[k++] = row[i + 1]; rgb[k++] = row[i + 2];
                            }
                        }
                        frame = frames; age = SystemClock.uptimeMillis() - latestAt;
                    }
                    deflater.reset();
                    deflater.setInput(rgb, 0, cw * ch * 3);
                    deflater.finish();
                    if (packed.length < cw * ch * 3 + 1024) packed = new byte[cw * ch * 3 + 1024];
                    int n = 0;
                    while (!deflater.finished()) n += deflater.deflate(packed, n, packed.length - n);
                    out.write(("F " + cw + " " + ch + " " + n + " " + age + " " + frame + "\n").getBytes());
                    out.write(packed, 0, n);
                } else if (a[0].equals("swipe")) {
                    swipe(f(a[1]), f(a[2]), f(a[3]), f(a[4]), Integer.parseInt(a[5]), Integer.parseInt(a[6]));
                    out.write("ok\n".getBytes());
                } else {
                    out.write("ok\n".getBytes());
                }
            } catch (Exception e) {
                out.write(("E " + e.toString().replace('\n', ' ') + "\n").getBytes());
            }
            out.flush();
        }
    }

    static float f(String s) { return Float.parseFloat(s); }

    // Screen mirror: Android 14+ has DisplayManager.createVirtualDisplay(name, w, h,
    // displayIdToMirror, surface) for the shell user; older versions bind a SurfaceControl
    // display to the main display's layer stack (what scrcpy does).
    static void mirror(Surface surface, int w, int h) throws Exception {
        try {
            Method m = android.hardware.display.DisplayManager.class.getMethod("createVirtualDisplay",
                    String.class, int.class, int.class, int.class, Surface.class);
            if (m.invoke(null, "bbhelper", w, h, 0, surface) != null) return;
        } catch (Exception e) {
            System.err.println("DisplayManager mirror failed: " + e);
        }
        Class<?> sc = Class.forName("android.view.SurfaceControl");
        IBinder token = (IBinder) sc.getMethod("createDisplay", String.class, boolean.class).invoke(null, "bbhelper", false);
        Object dmg = Class.forName("android.hardware.display.DisplayManagerGlobal").getMethod("getInstance").invoke(null);
        Object info = dmg.getClass().getMethod("getDisplayInfo", int.class).invoke(dmg, 0);
        int layerStack = info.getClass().getField("layerStack").getInt(info);
        sc.getMethod("openTransaction").invoke(null);
        try {
            sc.getMethod("setDisplaySurface", IBinder.class, Surface.class).invoke(null, token, surface);
            sc.getMethod("setDisplayProjection", IBinder.class, int.class, Rect.class, Rect.class)
                    .invoke(null, token, 0, new Rect(0, 0, w, h), new Rect(0, 0, w, h));
            sc.getMethod("setDisplayLayerStack", IBinder.class, int.class).invoke(null, token, layerStack);
        } finally {
            sc.getMethod("closeTransaction").invoke(null);
        }
    }

    static void setUpInput() throws Exception {
        Class<?> cls;
        try { cls = Class.forName("android.hardware.input.InputManagerGlobal"); }
        catch (ClassNotFoundException e) { cls = Class.forName("android.hardware.input.InputManager"); }
        inputManager = cls.getMethod("getInstance").invoke(null);
        inject = cls.getMethod("injectInputEvent", InputEvent.class, int.class);
    }

    static void send(long down, int action, float x, float y) throws Exception {
        MotionEvent e = MotionEvent.obtain(down, SystemClock.uptimeMillis(), action, x, y, 1f, 1f, 0, 1f, 1f, 0, 0);
        e.setSource(InputDevice.SOURCE_TOUCHSCREEN);
        // Moves don't wait; finger down and up wait until the game has handled them
        // (mode 2), so when "ok" comes back the drop has reached the game.
        inject.invoke(inputManager, e, action == MotionEvent.ACTION_MOVE ? 0 : 2);
        e.recycle();
    }

    static void swipe(float x1, float y1, float x2, float y2, int ms, int hold) throws Exception {
        long down = SystemClock.uptimeMillis();
        send(down, MotionEvent.ACTION_DOWN, x1, y1);
        long start = SystemClock.uptimeMillis();
        while (true) {
            SystemClock.sleep(8);
            float k = ms <= 0 ? 1f : Math.min(1f, (SystemClock.uptimeMillis() - start) / (float) ms);
            send(down, MotionEvent.ACTION_MOVE, x1 + (x2 - x1) * k, y1 + (y2 - y1) * k);
            if (k >= 1f) break;
        }
        long endHold = SystemClock.uptimeMillis() + hold;
        while (SystemClock.uptimeMillis() < endHold) {
            SystemClock.sleep(8);
            send(down, MotionEvent.ACTION_MOVE, x2, y2);
        }
        send(down, MotionEvent.ACTION_UP, x2, y2);
    }
}
