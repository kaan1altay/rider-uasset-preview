package dev.kaan.uassetpreview;

import com.google.gson.JsonParser;

import java.nio.file.Files;
import java.nio.file.Path;

/** Cross-layer checks against real packages exported by Test-Spatial.ps1. */
public final class SpatialInspectionTest {
    private static Inspection.Snapshot read(Path directory, String name) throws Exception {
        return Inspection.Snapshot.read(
                JsonParser.parseString(Files.readString(directory.resolve(name + ".json")))
                        .getAsJsonObject());
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    public static void main(String[] args) throws Exception {
        Path directory = Path.of(args[0]);
        var mapBefore = read(directory, "map-before");
        var mapAfter = read(directory, "map-after");
        var map = Inspection.compare(mapBefore, mapAfter);
        check(mapBefore.type().equals("World") && mapBefore.visual(), "Map viewport missing");
        check(map.added() == 2 && map.removed() == 0, "Added actor/component identities incorrect");
        check(
                map.visible(null, true, "Location").stream()
                        .anyMatch(r -> r.before().contains("100") && r.after().contains("500")),
                "Saved actor movement missing");
        var widgetBefore = read(directory, "widget-before");
        var widgetAfter = read(directory, "widget-after");
        var widget = Inspection.compare(widgetBefore, widgetAfter);
        check(
                widgetBefore.type().equals("WidgetBlueprint") && widgetBefore.visual(),
                "Widget viewport missing");
        check(
                widget.added() == 0 && widget.removed() == 0,
                "Widget identities changed between revisions");
        check(
                widget.visible(null, true, "Text").stream()
                        .anyMatch(r ->
                                r.before().contains("Before preview")
                                        && r.after().contains("After preview")),
                "Saved widget text missing");
        check(
                widget.visible(null, true, "Layout").stream()
                        .anyMatch(r -> r.before().contains("80") && r.after().contains("180")),
                "Canvas layout movement missing");
        System.out.println(
                "PASS: 7 real map/widget comparison checks; actor additions, movement, stable"
                    + " widget IDs, text and layout.");
    }
}
