package dev.kaan.uassetpreview;

import com.google.gson.*;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;

public final class InspectionTest {
    private static int checks;

    private static void check(boolean value, String message) {
        checks++;
        if (!value) throw new AssertionError(message);
    }

    private static JsonObject property(String key, String name, String value) {
        JsonObject p = new JsonObject();
        p.addProperty("key", key);
        p.addProperty("property", key);
        p.addProperty("name", name);
        p.addProperty("category", "Transform");
        p.addProperty("value", value);
        p.addProperty("diffValue", value);
        return p;
    }

    private static JsonObject fixture(String scale) {
        JsonObject j =
                JsonParser.parseString(
                                "{\"session\":\"s\",\"name\":\"BP_Test\",\"type\":\"Blueprint\",\"visual\":true,\"bounds\":[-50,-50,-50,50,50,50],\"components\":[{\"id\":\"/Temp/a.Body\",\"key\":\"scs:1234\",\"name\":\"Body\",\"type\":\"StaticMeshComponent\",\"parent\":\"asset\",\"bounds\":[-50,-50,-50,50,50,50]}],\"details\":{\"asset\":{\"properties\":[]},\"/Temp/a.Body\":{\"properties\":[]}},\"logic\":[]}")
                        .getAsJsonObject();
        j.getAsJsonObject("details")
                .getAsJsonObject("/Temp/a.Body")
                .getAsJsonArray("properties")
                .add(property("RelativeScale3D", "Scale", scale));
        return j;
    }

    public static void main(String[] args) throws Exception {
        check(
                Inspection.changedLines(
                                "Function: PrintString\nIn String: Before",
                                "Function: PrintString\nIn String: After")
                        .equals("In String: Before"),
                "Changed input was hidden by unchanged node details");
        check(
                Inspection.changedLines("Link\nLink", "Link").equals("Link"),
                "Repeated connection was lost");
        check(
                Inspection.changedLines("Link", "Link\nLink")
                        .equals("No additional pin/default lines"),
                "Removed connection summary was misleading");
        JsonObject a = fixture("(X=1,Y=1,Z=1)"), b = fixture("(X=2,Y=1,Z=1)");
        Inspection.Review review =
                Inspection.compare(Inspection.Snapshot.read(a), Inspection.Snapshot.read(b));
        check(review.settings() == 1, "Scale change not detected");
        check(
                review.added() == 0 && review.removed() == 0,
                "Matched component became added/removed");
        check(review.rows().get(0).before().contains("X=1"), "Before value lost");
        check(review.rows().get(0).after().contains("X=2"), "After value lost");
        check(
                review.rows().get(0).explanation().contains("Size multiplier"),
                "Friendly scale explanation missing");
        check(review.visible(null, true, "scale").size() == 1, "Search failed");
        check(review.visible(null, true, "does-not-exist").isEmpty(), "Search did not filter");
        check(
                review.visible("asset", true, "").isEmpty(),
                "Component filter leaked another component");
        check(review.visible(null, true, "Body").size() == 1, "Component-name search failed");
        ReviewTableModel table = new ReviewTableModel();
        table.replace(review.rows(), review);
        check(
                table.getValueAt(0, 1).toString().contains("Body"),
                "All-changes table omitted component ownership");
        JsonObject renamed = a.deepCopy();
        renamed.getAsJsonArray("components")
                .get(0)
                .getAsJsonObject()
                .addProperty("name", "MainBody");
        Inspection.Review rename =
                Inspection.compare(Inspection.Snapshot.read(a), Inspection.Snapshot.read(renamed));
        check(
                rename.added() == 0 && rename.removed() == 0,
                "Stable SCS GUID not preserved during rename");
        check(
                rename.rows().stream().anyMatch(r -> r.name().equals("Name")),
                "Rename not explained");
        JsonObject reinstance = a.deepCopy();
        JsonObject node = reinstance.getAsJsonArray("components").get(0).getAsJsonObject();
        node.addProperty("id", "/Temp/different.Body");
        JsonObject previousDetails =
                reinstance.getAsJsonObject("details").remove("/Temp/a.Body").getAsJsonObject();
        reinstance.getAsJsonObject("details").add("/Temp/different.Body", previousDetails);
        check(
                Inspection.compare(
                                        Inspection.Snapshot.read(a),
                                        Inspection.Snapshot.read(reinstance))
                                .settings()
                        == 0,
                "Temporary package IDs caused false changes");
        JsonObject canonical = a.deepCopy();
        canonical
                .getAsJsonObject("details")
                .getAsJsonObject("/Temp/a.Body")
                .getAsJsonArray("properties")
                .get(0)
                .getAsJsonObject()
                .addProperty("diffValue", "precise hidden change");
        Inspection.Review precise =
                Inspection.compare(
                        Inspection.Snapshot.read(a), Inspection.Snapshot.read(canonical));
        check(precise.settings() == 1, "Canonical value was ignored");
        check(
                precise.rows().get(0).after().equals("precise hidden change"),
                "Equal display labels concealed precise change");
        Inspection.Review add = Inspection.compare(null, Inspection.Snapshot.read(a)),
                remove = Inspection.compare(Inspection.Snapshot.read(a), null);
        check(add.added() == 1, "Added asset did not include its component");
        check(remove.removed() == 1, "Removed asset did not include its component");
        check(
                add.rows().stream().allMatch(r -> r.change() == Inspection.Change.ADDED),
                "Added asset contains false modifications");
        JsonObject missing = a.deepCopy();
        missing.getAsJsonObject("details").remove("/Temp/a.Body");
        Inspection.Review partial =
                Inspection.compare(Inspection.Snapshot.read(a), Inspection.Snapshot.read(missing));
        check(partial.limited(), "Partial export not flagged");
        check(partial.settings() == 0, "Missing inspection was treated as deleted values");
        JsonObject truncated = a.deepCopy();
        truncated
                .getAsJsonObject("details")
                .getAsJsonObject("/Temp/a.Body")
                .addProperty("truncated", true);
        truncated
                .getAsJsonObject("details")
                .getAsJsonObject("/Temp/a.Body")
                .add("properties", new JsonArray());
        check(
                Inspection.compare(Inspection.Snapshot.read(a), Inspection.Snapshot.read(truncated))
                                .settings()
                        == 0,
                "Truncated object falsely removed a property");
        JsonObject logic = a.deepCopy();
        JsonObject n =
                JsonParser.parseString(
                                "{\"key\":\"graph:node\",\"graph\":\"EventGraph\",\"name\":\"Print"
                                    + " String\",\"type\":\"K2Node_CallFunction\",\"value\":\"InString=Before\"}")
                        .getAsJsonObject();
        logic.getAsJsonArray("logic").add(n);
        JsonObject changedLogic = logic.deepCopy();
        changedLogic
                .getAsJsonArray("logic")
                .get(0)
                .getAsJsonObject()
                .addProperty("value", "InString=After");
        Inspection.Review graph =
                Inspection.compare(
                        Inspection.Snapshot.read(logic), Inspection.Snapshot.read(changedLogic));
        check(
                graph.logic() == 1 && graph.settings() == 0,
                "Logic changes were not separated from settings");
        check(graph.visible("logic", true, "").size() == 1, "Logic filter failed");
        JsonObject longLogic = logic.deepCopy();
        longLogic
                .getAsJsonArray("logic")
                .get(0)
                .getAsJsonObject()
                .addProperty("fingerprint", "different full value");
        longLogic.getAsJsonArray("logic").get(0).getAsJsonObject().addProperty("truncated", true);
        Inspection.Review tail =
                Inspection.compare(
                        Inspection.Snapshot.read(logic), Inspection.Snapshot.read(longLogic));
        check(tail.logic() == 1 && tail.limited(), "Long graph value change was hidden");
        check(
                tail.visible("logic", true, "").get(0).after().contains("beyond"),
                "Long node change is not explained");
        double[] combined =
                Inspection.combineBounds(
                        Inspection.Snapshot.read(a), Inspection.Snapshot.read(b), "asset");
        check(combined[0] == -50 && combined[3] == 50, "Shared camera bounds changed");
        b.getAsJsonArray("bounds").set(3, new JsonPrimitive(200));
        combined =
                Inspection.combineBounds(
                        Inspection.Snapshot.read(a), Inspection.Snapshot.read(b), "unknown");
        check(combined[3] == 200, "Shared camera normalized away a size difference");
        check(
                AssetKnowledge.explain("MaxWalkSpeed", "", "Movement")
                        .contains("centimeters per second"),
                "Movement units missing");
        check(
                AssetKnowledge.explain("CustomProperty", "Owner-provided explanation.", "Custom")
                        .equals("Owner-provided explanation."),
                "Metadata explanation lost");
        java.awt.image.BufferedImage imageA =
                new java.awt.image.BufferedImage(2, 2, java.awt.image.BufferedImage.TYPE_INT_RGB);
        java.awt.image.BufferedImage imageB =
                new java.awt.image.BufferedImage(2, 2, java.awt.image.BufferedImage.TYPE_INT_RGB);
        imageB.setRGB(1, 1, 0xFFFFFF);
        check(
                RevisionImages.difference(imageA, imageB).getRGB(1, 1)
                        != RevisionImages.difference(imageA, imageB).getRGB(0, 0),
                "Visual difference mask did not highlight a changed pixel");
        check(
                RevisionImages.difference(
                                        new java.awt.image.BufferedImage(
                                                1, 1, java.awt.image.BufferedImage.TYPE_INT_RGB),
                                        imageB)
                                .getWidth()
                        == 2,
                "Different image sizes were cropped during comparison");
        javax.swing.JLabel untrusted = UiText.plain(new javax.swing.JLabel());
        untrusted.setText("<html><img src='https://invalid.example/asset'>");
        check(
                Boolean.TRUE.equals(untrusted.getClientProperty("html.disable")),
                "Asset strings may be interpreted as HTML");
        check(
                untrusted.getClientProperty(javax.swing.plaf.basic.BasicHTML.propertyKey) == null,
                "HTML renderer was created for asset text");

        Path temp = Files.createTempDirectory("uasset-revisions-");
        try {
            byte[] binary = {(byte) 0xC1, (byte) 0x83, 0x2A, (byte) 0x9E, 1, 2, 3, 4};
            check(
                    Arrays.equals(binary, RevisionBytes.resolve(binary, temp)),
                    "Binary revision was changed");
            String oid =
                    HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(binary));
            byte[] pointer =
                    ("version https://git-lfs.github.com/spec/v1\noid sha256:"
                                    + oid
                                    + "\nsize "
                                    + binary.length
                                    + "\n")
                            .getBytes(StandardCharsets.UTF_8);
            Path lfs =
                    temp.resolve(
                            ".git/lfs/objects/"
                                    + oid.substring(0, 2)
                                    + "/"
                                    + oid.substring(2, 4)
                                    + "/"
                                    + oid);
            Files.createDirectories(lfs.getParent());
            Files.write(lfs, binary);
            check(
                    Arrays.equals(binary, RevisionBytes.resolve(pointer, temp)),
                    "LFS cache not used");
            Files.write(lfs, new byte[binary.length]);
            expectFailure(() -> RevisionBytes.resolve(pointer, temp), "SHA-256");
            Files.delete(lfs);
            expectFailure(() -> RevisionBytes.resolve(pointer, temp), "not cached");
            expectFailure(
                    () ->
                            RevisionBytes.resolve(
                                    "not an asset".getBytes(StandardCharsets.UTF_8), temp),
                    "not a readable Unreal");
            expectFailure(() -> RevisionBytes.resolve(new byte[0], temp), "not a readable Unreal");
        } finally {
            try (var paths = Files.walk(temp)) {
                for (Path path : paths.sorted(Comparator.reverseOrder()).toList())
                    Files.delete(path);
            }
        }
        if (args.length == 2) {
            Inspection.Snapshot nativeBefore =
                    Inspection.Snapshot.read(
                            JsonParser.parseString(Files.readString(Path.of(args[0])))
                                    .getAsJsonObject());
            Inspection.Snapshot nativeAfter =
                    Inspection.Snapshot.read(
                            JsonParser.parseString(Files.readString(Path.of(args[1])))
                                    .getAsJsonObject());
            Inspection.Review nativeReview = Inspection.compare(nativeBefore, nativeAfter);
            check(
                    nativeReview.added() == 1 && nativeReview.removed() == 0,
                    "Native fixture component matching failed");
            check(
                    nativeReview.logic() == 1,
                    "Native node comparison detected false changes or missed the changed default");
            check(
                    nativeReview.visible(null, true, "Body").stream()
                            .anyMatch(r -> r.name().contains("Scale")),
                    "Native scale change missing from review");
            check(
                    nativeReview.components().stream()
                            .filter(c -> c.name().equals("Body"))
                            .allMatch(c -> c.change() == Inspection.Change.MODIFIED),
                    "Native body was treated as added/removed");
            System.out.println(
                    "Native review: "
                            + nativeReview.settings()
                            + " setting differences, "
                            + nativeReview.logic()
                            + " changed logic node, +"
                            + nativeReview.added()
                            + "/−"
                            + nativeReview.removed()
                            + " components.");
            for (Inspection.Row r : nativeReview.visible(null, true, "Body"))
                System.out.println(
                        "  "
                                + nativeReview.owner(r)
                                + " / "
                                + r.name()
                                + ": "
                                + r.before()
                                + " → "
                                + r.after());
        }
        System.out.println(
                "PASS: "
                        + checks
                        + " review checks; stable identities, additions/removals, precise values,"
                        + " logic, partial coverage, shared bounds and verified LFS revisions.");
    }

    @FunctionalInterface
    interface Task {
        void run() throws Exception;
    }

    private static void expectFailure(Task task, String message) throws Exception {
        try {
            task.run();
            throw new AssertionError("Expected revision failure");
        } catch (IOException e) {
            check(e.getMessage().contains(message), "Unexpected failure: " + e);
        }
    }
}
