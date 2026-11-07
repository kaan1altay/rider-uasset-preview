package dev.kaan.uassetpreview;

import java.awt.image.BufferedImage;

/** Pixel comparison is deliberately separate from semantic property comparison. */
final class RevisionImages {
    static BufferedImage difference(BufferedImage a, BufferedImage b) {
        if (a == null || b == null) return null;
        int w = Math.max(a.getWidth(), b.getWidth()), h = Math.max(a.getHeight(), b.getHeight());
        BufferedImage out = new BufferedImage(w, h, BufferedImage.TYPE_INT_RGB);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int av = a.getRGB(x * a.getWidth() / w, y * a.getHeight() / h),
                        bv = b.getRGB(x * b.getWidth() / w, y * b.getHeight() / h);
                int delta =
                        Math.max(
                                Math.abs(((av >> 16) & 255) - ((bv >> 16) & 255)),
                                Math.max(
                                        Math.abs(((av >> 8) & 255) - ((bv >> 8) & 255)),
                                        Math.abs((av & 255) - (bv & 255))));
                int gray = ((((bv >> 16) & 255) + ((bv >> 8) & 255) + (bv & 255)) / 3) / 4;
                out.setRGB(
                        x,
                        y,
                        delta > 20
                                ? (Math.min(255, 90 + delta) << 16)
                                        | (Math.min(255, 70 + delta / 2) << 8)
                                        | 50
                                : (gray << 16) | (gray << 8) | gray);
            }
        return out;
    }

    private RevisionImages() {}
}
