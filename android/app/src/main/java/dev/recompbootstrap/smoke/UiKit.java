package dev.recompbootstrap.smoke;

import android.content.pm.PackageManager;
import android.view.InputDevice;
import android.content.Context;
import android.content.res.AssetManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.os.SystemClock;
import android.view.MotionEvent;
import android.view.View;
import java.io.File;

/**
 * The Android side of the menu's look (local-analysis/menu-redesign/DESIGN.md): the face, the painter of a Scene and one view that
 * shows a Scene. Everything is drawn in code; the one file it reads is the bundled typeface (assets/fonts/NunitoSans-VF.ttf, SIL OFL
 * 1.1, FontNotice) and the one picture it can show is the player's own last game frame (files/settings/menu-backdrop.png, written by
 * the game activity when the in-game menu opens). What is drawn and where is decided by MenuScenes and MenuLayout, which are plain
 * Java and tested on the host.
 */
final class UiKit {
    /** The window colour behind the menu (the activity sets it before the first frame is drawn). */
    static final int BACKGROUND = 0xFF12171F;

    /** The backdrop picture, relative to the app's files folder; the first of the two that exists is used. */
    static final String[] BACKDROP_FILES = {"settings/menu-backdrop.jpg", "settings/menu-backdrop.png"};

    private UiKit() { }

    /** The bundled face: Nunito Sans, a variable font whose DEFAULT instance is weight 200, so every weight is set explicitly. */
    static final String FONT_ASSET = "fonts/NunitoSans-VF.ttf";
    static final String WEIGHT_REGULAR = "'wght' 400", WEIGHT_BOLD = "'wght' 700", WEIGHT_HEAVY = "'wght' 900";

    /** The menu's typefaces: Nunito Sans at 400 (REGULAR, TITLE, CAPS), 700 (BOLD) and 900 (HEAVY); the system's sans without the file. */
    static final class Faces {
        final Typeface regular, bold, heavy;

        Faces(AssetManager assets) {
            regular = load(assets, WEIGHT_REGULAR, Typeface.create("sans-serif", Typeface.NORMAL));
            bold = load(assets, WEIGHT_BOLD, Typeface.create("sans-serif", Typeface.BOLD));
            heavy = load(assets, WEIGHT_HEAVY, Typeface.create("sans-serif-black", Typeface.BOLD));
        }

        private static Typeface load(AssetManager assets, String weight, Typeface fallback) {
            try {
                Typeface face = new Typeface.Builder(assets, FONT_ASSET).setFontVariationSettings(weight).build();
                return face != null ? face : fallback;
            } catch (RuntimeException unreadable) {
                return fallback;
            }
        }

        Typeface of(int face) {
            return face == Scene.BOLD ? bold : face == Scene.HEAVY ? heavy : regular;
        }

        private static Faces shared;

        /** One set per process (a Typeface is immutable and the file is read once). */
        static synchronized Faces get(Context context) {
            if (shared == null) shared = new Faces(context.getApplicationContext().getAssets());
            return shared;
        }
    }

    /** round(u * panelPixels), at least 1. */
    static int px(float u, int panelPixels) {
        return Math.max(1, Math.round(u * panelPixels));
    }

    /**
     * Reads the user's backdrop picture, much reduced (MenuLayout.backdropSample) and then doubled with filtering
     * (MenuLayout.backdropDoublings) so that it is drawn smoothly blurred, or returns null when there is no such
     * file, it is not a picture, or it came out larger than MenuLayout.backdropFits allows. Only reads; call it off
     * the UI thread.
     */
    static Bitmap loadBackdrop(File filesDir) {
        for (String name : BACKDROP_FILES) {
            File file = new File(filesDir, name);
            if (!file.isFile()) continue;
            try {
                BitmapFactory.Options options = new BitmapFactory.Options();
                options.inJustDecodeBounds = true;
                BitmapFactory.decodeFile(file.getPath(), options);
                if (options.outWidth <= 0 || options.outHeight <= 0) continue;
                options.inSampleSize = MenuLayout.backdropSample(options.outWidth, options.outHeight);
                options.inJustDecodeBounds = false;
                Bitmap picture = BitmapFactory.decodeFile(file.getPath(), options);
                if (picture == null) continue;
                // A decoder that did not reduce the picture as asked: too large to hold or to draw.
                if (!MenuLayout.backdropFits(picture.getWidth(), picture.getHeight())) continue;
                for (int steps = MenuLayout.backdropDoublings(picture.getWidth(), picture.getHeight()); steps > 0; --steps) {
                    picture = Bitmap.createScaledBitmap(picture, picture.getWidth() * 2, picture.getHeight() * 2, true);
                }
                return picture;
            } catch (RuntimeException | OutOfMemoryError unreadable) {
                // The menu then keeps its plain backdrop.
            }
        }
        return null;
    }

    interface Listener {
        /** A tap on a touch rectangle of the scene (MenuLayout.HIT_*). */
        void onHit(int id);
        /** A vertical drag by this many text lines; positive moves the text up. */
        void onScroll(int lines);
        /** The view got its size, or another one: a screen that builds its scene with SceneView.frame() makes it again. */
        default void onResized() {
        }
    }

    /** The name of the first real gamepad or joystick Android reports, or null. */
    static String controllerName() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice device = InputDevice.getDevice(id);
            if (device != null && !device.isVirtual() && (device.supportsSource(InputDevice.SOURCE_GAMEPAD)
                    || device.supportsSource(InputDevice.SOURCE_JOYSTICK))) {
                return device.getName();
            }
        }
        return null;
    }

    /** True on a touch screen (Android's FEATURE_TOUCHSCREEN): where the X and check icons can stand in for the prompts. */
    static boolean touchScreen(Context context) {
        return context.getPackageManager().hasSystemFeature(PackageManager.FEATURE_TOUCHSCREEN);
    }

    /**
     * Paints a Scene: its panel (w x h panel pixels) scaled by u and centred in a view of viewW x viewH; the backdrop and a FILL cover
     * the whole view. Used by the launcher's SceneView and by the in-game menus' views alike. Also the text measure of MenuScenes.
     */
    static final class Painter implements Scene.Measure {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.SUBPIXEL_TEXT_FLAG | Paint.LINEAR_TEXT_FLAG
                | Paint.FILTER_BITMAP_FLAG);
        private final Paint measurePaint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.SUBPIXEL_TEXT_FLAG | Paint.LINEAR_TEXT_FLAG);
        private final Paint.FontMetrics metrics = new Paint.FontMetrics();
        private final Path path = new Path();
        private final Rect source = new Rect();
        private final RectF window = new RectF();
        private final Faces faces;

        Painter(Context context) {
            faces = Faces.get(context);
        }

        private void face(Paint target, int sizePx, int face) {
            target.setTextSize(sizePx);
            target.setTypeface(faces.of(face));
        }

        @Override public float width(String text, int sizePx, int face) {
            face(measurePaint, sizePx, face);
            return measurePaint.measureText(text);
        }

        static float left(int viewW, int panelW, float u) {
            return (viewW - panelW * u) / 2f;
        }

        static float top(int viewH, int panelH, float u) {
            return (viewH - panelH * u) / 2f;
        }

        /** Draws the scene; fade is the opacity of the CONTENT layer (MenuLayout.fadeAlpha). */
        void draw(Canvas canvas, Scene scene, int viewW, int viewH, int panelW, int panelH, float u, Bitmap backdrop, float fade) {
            float left = left(viewW, panelW, u), top = top(viewH, panelH, u);
            canvas.save();
            canvas.translate(left, top);
            canvas.scale(u, u);
            // The whole view in the panel's own pixels: the backdrop and a FILL cover it, not only the panel.
            window.set(-left / u, -top / u, (viewW - left) / u, (viewH - top) / u);
            for (int i = 0; i < scene.items.size(); ++i) {
                Scene.Item item = scene.items.get(i);
                paint.setShader(null);
                paint.setStyle(Paint.Style.FILL);
                paint.setColor(item.color);
                if (item.layer == Scene.CONTENT && fade < 1f) paint.setAlpha(Math.round((item.color >>> 24) * fade));
                switch (item.type) {
                    case Scene.RECT:
                        canvas.drawRect(item.x, item.y, item.x + item.w, item.y + item.h, paint);
                        break;
                    case Scene.FILL:
                        canvas.drawRect(window, paint);
                        break;
                    case Scene.BACKDROP:
                        if (backdrop != null) {
                            int[] part = MenuLayout.cropToFill(backdrop.getWidth(), backdrop.getHeight(), viewW, viewH);
                            source.set(part[0], part[1], part[2], part[3]);
                            paint.setColor(0xFFFFFFFF);
                            canvas.drawBitmap(backdrop, source, window, paint);
                            paint.setColor(MenuLayout.BACKDROP_SHADE);
                        } else {
                            paint.setShader(new LinearGradient(0, window.top, 0, window.bottom, item.color, item.color2,
                                    Shader.TileMode.CLAMP));
                        }
                        canvas.drawRect(window, paint);
                        break;
                    case Scene.ROUND:
                        canvas.drawRoundRect(item.x, item.y, item.x + item.w, item.y + item.h, item.stroke, item.stroke, paint);
                        break;
                    case Scene.DISC:
                        canvas.drawCircle(item.x + item.w / 2f, item.y + item.h / 2f, item.w / 2f, paint);
                        break;
                    case Scene.RING:
                        paint.setStyle(Paint.Style.STROKE);
                        paint.setStrokeWidth(item.stroke);
                        canvas.drawCircle(item.x + item.w / 2f, item.y + item.h / 2f, item.w / 2f, paint);
                        break;
                    case Scene.POLY:
                    case Scene.LINE:
                        path.rewind();
                        path.moveTo(item.points[0], item.points[1]);
                        for (int p = 2; p + 1 < item.points.length; p += 2) path.lineTo(item.points[p], item.points[p + 1]);
                        if (item.type == Scene.POLY) {
                            path.close();
                        } else {
                            paint.setStyle(Paint.Style.STROKE);
                            paint.setStrokeWidth(item.stroke);
                            paint.setStrokeJoin(Paint.Join.MITER);
                            paint.setStrokeCap(Paint.Cap.BUTT);
                        }
                        canvas.drawPath(path, paint);
                        break;
                    default: {   // TEXT: centred vertically in its box
                        face(paint, item.size, item.face);
                        paint.getFontMetrics(metrics);
                        float baseline = item.y + item.h / 2f - (metrics.ascent + metrics.descent) / 2f;
                        float width = paint.measureText(item.text);
                        float x = item.align == Scene.LEFT ? item.x
                                : item.align == Scene.RIGHT ? item.x + item.w - width : item.x + (item.w - width) / 2f;
                        if (item.shadow) {
                            float offset = MenuLayout.shadowOffset(item.size);
                            paint.setShadowLayer(MenuLayout.shadowBlur(item.size), offset, offset, MenuLayout.SHADOW);
                        }
                        canvas.drawText(item.text, x, baseline, paint);
                        paint.clearShadowLayer();
                    }
                }
            }
            canvas.restore();
        }
    }

    /**
     * Shows a Scene scaled from the 1920 x 1080 design panel to the window and centred in it. It never takes Android's own focus, so
     * the system draws no highlight of its own: what is highlighted is in the scene.
     */
    static final class SceneView extends View implements Scene.Measure {
        private final Painter painter;
        private final float u;
        private final Listener listener;
        private Scene scene;
        private Bitmap backdrop;
        private long contentSince;
        private float downX, downY, dragFrom;
        private boolean dragging;

        SceneView(Context context, float u, Listener listener) {
            super(context);
            this.painter = new Painter(context);
            this.u = u;
            this.listener = listener;
            setFocusable(false);
            setFocusableInTouchMode(false);
            setDefaultFocusHighlightEnabled(false);
        }

        /**
         * Shows a scene. When its content is another page than before, the pane's texts fade in; input is not held
         * back. A scene equal to the one shown is not drawn again (Scene.needsDraw).
         */
        void show(Scene next) {
            if (!Scene.needsDraw(scene, next)) return;
            if (scene == null || !scene.contentKey.equals(next.contentKey)) contentSince = SystemClock.uptimeMillis();
            scene = next;
            invalidate();
        }

        /** The player's backdrop picture (UiKit.loadBackdrop), or null for the plain backdrop. */
        void setBackdrop(Bitmap picture) {
            backdrop = picture;
            invalidate();
        }

        @Override public float width(String text, int sizePx, int face) {
            return painter.width(text, sizePx, face);
        }

        /** The frame for this view: DEFAULT, or one that fills a landscape screen taller than 16:9 (MenuLayout.Frame.screen). */
        MenuLayout.Frame frame() {
            return MenuLayout.Frame.screen(MenuLayout.panelHeight(getWidth(), getHeight(), u));
        }

        /** The prompts as the X and check icons: a touch screen with no gamepad connected (owner 2026-10-25). */
        boolean touchIcons() {
            return touchScreen(getContext()) && controllerName() == null;
        }

        @Override protected void onSizeChanged(int w, int h, int oldW, int oldH) {
            if (w != oldW || h != oldH) listener.onResized();
        }

        private float left() {
            return Painter.left(getWidth(), scene == null ? MenuLayout.W : scene.panelW, u);
        }

        private float top() {
            return Painter.top(getHeight(), scene == null ? MenuLayout.H : scene.panelH, u);
        }

        @Override protected void onDraw(Canvas canvas) {
            canvas.drawColor(MenuLayout.BACKDROP_BOTTOM);
            if (scene == null) return;
            float fade = MenuLayout.fadeAlpha(SystemClock.uptimeMillis() - contentSince);
            painter.draw(canvas, scene, getWidth(), getHeight(), scene.panelW, scene.panelH, u, backdrop, fade);
            if (fade < 1f) postInvalidateOnAnimation();
        }

        @Override public boolean onTouchEvent(MotionEvent event) {
            if (scene == null) return false;
            float x = (event.getX() - left()) / u, y = (event.getY() - top()) / u;
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    downX = x;
                    downY = dragFrom = y;
                    dragging = false;
                    return true;
                case MotionEvent.ACTION_MOVE: {
                    // A finger that travels more than a text line scrolls and is no longer a tap.
                    int lines = (int) ((dragFrom - y) / MenuLayout.TEXT_LINE);
                    if (lines != 0) {
                        dragging = true;
                        dragFrom -= lines * MenuLayout.TEXT_LINE;
                        listener.onScroll(lines);
                    }
                    return true;
                }
                case MotionEvent.ACTION_UP:
                    if (!dragging) {
                        int id = scene.hitAt(downX, downY);
                        if (id >= 0 && id == scene.hitAt(x, y)) listener.onHit(id);
                    }
                    return true;
                default:
                    return true;
            }
        }
    }
}
