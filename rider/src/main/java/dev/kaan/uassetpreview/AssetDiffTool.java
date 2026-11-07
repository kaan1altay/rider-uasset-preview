package dev.kaan.uassetpreview;

import com.intellij.diff.*;
import com.intellij.diff.contents.*;
import com.intellij.diff.requests.*;
import com.intellij.diff.util.DiffUserDataKeysEx;
import com.intellij.openapi.fileEditor.OpenFileDescriptor;
import com.intellij.openapi.vfs.VirtualFile;

import org.jetbrains.annotations.NotNull;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

/** Native Rider diff integration, including historical/non-local VCS files. */
public final class AssetDiffTool implements FrameDiffTool {
    @Override
    public @NotNull String getName() {
        return "Unreal asset review";
    }

    @Override
    public boolean canShow(@NotNull DiffContext context, @NotNull DiffRequest request) {
        if (!(request instanceof ContentDiffRequest content) || content.getContents().size() != 2)
            return false;
        boolean found = false;
        for (DiffContent side : content.getContents()) {
            if (side instanceof EmptyContent) continue;
            if (!name(side).toLowerCase(Locale.ROOT).endsWith(".uasset")) return false;
            if (!(side instanceof FileContent) && !(side instanceof DocumentContent)) return false;
            found = true;
        }
        return found;
    }

    @Override
    public @NotNull DiffViewer createComponent(
            @NotNull DiffContext context, @NotNull DiffRequest request) {
        return new AssetDiffViewer(context.getProject(), (ContentDiffRequest) request);
    }

    static String name(DiffContent side) {
        String hint = side.getUserData(DiffUserDataKeysEx.FILE_NAME);
        if (hint != null && !hint.isBlank()) return hint;
        VirtualFile file = file(side);
        return file == null ? "" : file.getName();
    }

    static VirtualFile file(DiffContent side) {
        if (side instanceof FileContent content) return content.getFile();
        if (side instanceof DocumentContent content) return content.getHighlightFile();
        return null;
    }

    static Path originalPath(DiffContent side) {
        var revision = side.getUserData(DiffVcsDataKeys.REVISION_INFO);
        if (revision != null) {
            Path p = path(revision.first.getPath());
            if (p != null) return p;
        }
        if (side.getNavigatable() instanceof OpenFileDescriptor descriptor) {
            Path p = path(descriptor.getFile().getPath());
            if (p != null) return p;
        }
        if (side instanceof FileContentImpl content) {
            Path p = path(content.getFilePath());
            if (p != null) return p;
        }
        VirtualFile file = file(side);
        return file == null ? null : path(file.getPath());
    }

    private static Path path(String value) {
        if (value == null) return null;
        try {
            Path p = Path.of(value);
            return p.isAbsolute() && value.toLowerCase(Locale.ROOT).endsWith(".uasset") ? p : null;
        } catch (InvalidPathException error) {
            return null;
        }
    }

    static byte[] bytes(DiffContent side, Path project) throws IOException {
        if (side instanceof FileContent content) {
            if (content.getFile().getLength() > RevisionBytes.LIMIT)
                throw new IOException("This asset revision exceeds the 128 MB inspection limit.");
            return RevisionBytes.resolve(content.getFile().contentsToByteArray(), project);
        }
        if (side instanceof DocumentContent content)
            return RevisionBytes.resolve(
                    content.getDocument().getText().getBytes(StandardCharsets.UTF_8), project);
        throw new IOException("This diff does not provide the original asset bytes.");
    }
}
