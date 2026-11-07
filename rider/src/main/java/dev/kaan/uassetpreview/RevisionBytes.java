package dev.kaan.uassetpreview;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.security.*;
import java.util.*;

/** Reads revision bytes without substituting the working-tree version. */
final class RevisionBytes {
    static final long LIMIT = 128L * 1024 * 1024;

    static byte[] resolve(byte[] bytes, Path project) throws IOException {
        if (bytes.length > LIMIT)
            throw new IOException("This revision exceeds the 128 MB inspection limit.");
        String prefix = new String(bytes, 0, Math.min(bytes.length, 1024), StandardCharsets.UTF_8);
        if (prefix.startsWith("version https://git-lfs.github.com/spec/v1")) {
            String oid = null;
            long size = -1;
            for (String line : prefix.lines().toList()) {
                if (line.matches("oid sha256:[a-f0-9]{64}")) oid = line.substring(11);
                if (line.matches("size [0-9]+"))
                    try {
                        size = Long.parseLong(line.substring(5));
                    } catch (NumberFormatException ignored) {
                    }
            }
            if (oid == null || size < 4 || size > LIMIT)
                throw new IOException(
                        "The Git LFS pointer is invalid or exceeds the inspection size limit.");
            Path git = gitDirectory(project);
            Path object =
                    git == null
                            ? null
                            : git.resolve(
                                    "lfs/objects/"
                                            + oid.substring(0, 2)
                                            + "/"
                                            + oid.substring(2, 4)
                                            + "/"
                                            + oid);
            if (object == null || !Files.isRegularFile(object) || Files.size(object) != size)
                throw new IOException(
                        "This revision contains a Git LFS pointer, and its asset data is not cached"
                            + " locally. Fetch the LFS objects for this revision, then reopen the"
                            + " diff. The working copy has not been used as a substitute.");
            bytes = Files.readAllBytes(object);
            try {
                if (!HexFormat.of()
                        .formatHex(MessageDigest.getInstance("SHA-256").digest(bytes))
                        .equals(oid))
                    throw new IOException("The cached LFS object failed its SHA-256 check.");
            } catch (NoSuchAlgorithmException error) {
                throw new IOException(error);
            }
        }
        if (bytes.length < 4
                || (bytes[0] & 255) != 0xC1
                || (bytes[1] & 255) != 0x83
                || (bytes[2] & 255) != 0x2A
                || (bytes[3] & 255) != 0x9E)
            throw new IOException(
                    "This revision is not a readable Unreal asset package. Fetch its original"
                        + " binary data and try again.");
        return bytes;
    }

    private static Path gitDirectory(Path start) throws IOException {
        for (Path dir = start; dir != null; dir = dir.getParent()) {
            Path git = dir.resolve(".git");
            if (Files.isDirectory(git)) return git;
            if (Files.isRegularFile(git) && Files.size(git) < 8192) {
                String link = Files.readString(git).strip();
                if (!link.startsWith("gitdir: ")) return null;
                Path result = dir.resolve(link.substring(8)).normalize();
                Path common = result.resolve("commondir");
                return Files.isRegularFile(common) && Files.size(common) < 8192
                        ? result.resolve(Files.readString(common).strip()).normalize()
                        : result;
            }
        }
        return null;
    }

    private RevisionBytes() {}
}
