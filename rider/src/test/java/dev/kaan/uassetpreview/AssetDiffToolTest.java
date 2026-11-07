package dev.kaan.uassetpreview;

import com.intellij.diff.*;
import com.intellij.diff.contents.*;
import com.intellij.diff.requests.*;
import com.intellij.diff.util.DiffUserDataKeysEx;
import com.intellij.openapi.editor.Document;
import com.intellij.openapi.fileTypes.FileType;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.util.Pair;
import com.intellij.openapi.vcs.FilePath;
import com.intellij.openapi.vcs.history.VcsRevisionNumber;

import java.lang.reflect.Proxy;
import java.nio.file.Path;
import java.util.List;

/** Exercises Rider's real diff request/content types, without starting an IDE. */
public final class AssetDiffToolTest {
    private static int checks;

    private static void check(boolean value, String message) {
        checks++;
        if (!value) throw new AssertionError(message);
    }

    private static final class PointerContent extends DiffContentBase implements DocumentContent {
        @Override
        public Document getDocument() {
            return null;
        }

        @Override
        public FileType getContentType() {
            return null;
        }
    }

    public static void main(String[] args) throws Exception {
        // Exercise the packaged descriptor as well as the adapter: a valid implementation
        // registered under a misspelled extension point is silently absent in a real IDE.
        var descriptor =
                javax.xml.parsers.DocumentBuilderFactory.newInstance()
                        .newDocumentBuilder()
                        .parse(AssetDiffToolTest.class.getResourceAsStream("/META-INF/plugin.xml"));
        var registration = descriptor.getElementsByTagName("diff.DiffTool");
        check(registration.getLength() == 1, "Native diff extension point is not registered");
        check(
                ((org.w3c.dom.Element) registration.item(0))
                        .getAttribute("implementation")
                        .equals(AssetDiffTool.class.getName()),
                "Native diff implementation is not registered");
        AssetDiffTool tool = new AssetDiffTool();
        DiffContext context =
                new DiffContext() {
                    @Override
                    public boolean isWindowFocused() {
                        return false;
                    }

                    @Override
                    public Project getProject() {
                        return null;
                    }

                    @Override
                    public void requestFocusInWindow() {}

                    @Override
                    public boolean isFocusedInWindow() {
                        return false;
                    }
                };
        PointerContent old = new PointerContent();
        old.putUserData(DiffUserDataKeysEx.FILE_NAME, "BP_Örnek.uasset");
        PointerContent next = new PointerContent();
        next.putUserData(DiffUserDataKeysEx.FILE_NAME, "BP_Örnek.uasset");
        EmptyContent empty = new EmptyContent();
        PointerContent map = new PointerContent();
        map.putUserData(DiffUserDataKeysEx.FILE_NAME, "Level.UMAP");
        check(
                tool.canShow(context, new SimpleDiffRequest("Map", map, map, "Before", "After")),
                "Map revisions not accepted");
        check(
                tool.canShow(
                        context,
                        new SimpleDiffRequest("Deleted map", map, empty, "Before", "After")),
                "Deleted map not accepted");
        check(!AssetDiffTool.isPackage("Level.umap.bak"), "Unrelated backup intercepted");
        check(
                tool.canShow(
                        context,
                        new SimpleDiffRequest("Changed", old, next, "HEAD", "Working copy")),
                "Changed VCS asset not accepted");
        check(
                tool.canShow(
                        context, new SimpleDiffRequest("Added", empty, next, "Before", "After")),
                "Added asset not accepted");
        check(
                tool.canShow(
                        context, new SimpleDiffRequest("Deleted", old, empty, "Before", "After")),
                "Deleted LFS pointer not accepted");
        check(
                !tool.canShow(
                        context, new SimpleDiffRequest("Empty", empty, empty, "Before", "After")),
                "Empty diff claimed by plugin");
        PointerContent text = new PointerContent();
        text.putUserData(DiffUserDataKeysEx.FILE_NAME, "Notes.txt");
        check(
                !tool.canShow(
                        context, new SimpleDiffRequest("Text", text, text, "Before", "After")),
                "Text diff intercepted");
        check(
                !tool.canShow(
                        context,
                        new SimpleDiffRequest(
                                "Merge",
                                List.of(old, next, old),
                                List.of("Left", "Base", "Right"))),
                "Three-way merge intercepted");
        String original = "C:\\Project\\Content\\BP_Örnek.uasset";
        FilePath path =
                (FilePath)
                        Proxy.newProxyInstance(
                                AssetDiffToolTest.class.getClassLoader(),
                                new Class<?>[] {FilePath.class},
                                (proxy, method, values) ->
                                        switch (method.getName()) {
                                            case "getPath", "getPresentableUrl", "toString" ->
                                                    original;
                                            case "getName" -> "BP_Örnek.uasset";
                                            case "isDirectory", "isNonLocal", "isUnder" -> false;
                                            default -> null;
                                        });
        old.putUserData(DiffVcsDataKeys.REVISION_INFO, Pair.create(path, VcsRevisionNumber.NULL));
        check(
                AssetDiffTool.originalPath(old).equals(Path.of(original)),
                "Original path of deleted LFS revision lost");
        check(AssetDiffTool.originalPath(text) == null, "Missing original path was guessed");
        System.out.println(
                "PASS: "
                        + checks
                        + " Rider diff adapter checks; changed/added/deleted assets, text/merge"
                        + " fallback and original VCS revision paths.");
    }
}
