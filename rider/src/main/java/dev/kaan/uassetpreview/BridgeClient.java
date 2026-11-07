package dev.kaan.uassetpreview;

import com.google.gson.*;
import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

/** Transport deliberately independent of IDE APIs, so the real HTTP contract can be tested. */
final class BridgeClient {
    static final int PROTOCOL = 1;
    private static final int MAX_JSON = 4 * 1024 * 1024;
    private static final int MAX_IMAGE = 16 * 1024 * 1024;
    private final URI endpoint;
    private final String token;

    BridgeClient(URI endpoint, String token) throws IOException {
        if (!"http".equals(endpoint.getScheme()) || !"127.0.0.1".equals(endpoint.getHost())
                || endpoint.getPort() < 1024 || endpoint.getPort() > 65535
                || endpoint.getRawUserInfo() != null || endpoint.getRawQuery() != null
                || endpoint.getRawFragment() != null || !"".equals(endpoint.getPath())) {
            throw new IOException("The bridge address must be http://127.0.0.1:<port>.");
        }
        if (token == null || !token.matches("[a-fA-F0-9]{64}"))
            throw new IOException("Invalid bridge session token. Restart the Unreal Editor bridge.");
        this.endpoint = endpoint;
        this.token = token;
    }

    static Path discover(Path asset, Path projectBase) throws IOException {
        // Plugin/Engine assets can be outside the project; try Rider's project first.
        List<Path> starts = new ArrayList<>();
        if (projectBase != null) starts.add(projectBase);
        starts.add(asset.toAbsolutePath().getParent());
        for (Path start : starts) {
            for (Path dir = start; dir != null; dir = dir.getParent()) {
                Path descriptor = dir.resolve("Saved/RiderAssetPreview/session.json");
                if (Files.isRegularFile(descriptor)) return descriptor;
            }
        }
        throw new IOException("No Unreal bridge session found. Enable RiderAssetPreview in the open project, then reconnect.");
    }

    static BridgeClient fromDescriptor(Path path) throws IOException {
        if (Files.size(path) > 8192) throw new IOException("Bridge session file is too large.");
        try {
            JsonObject data = JsonParser.parseString(Files.readString(path)).getAsJsonObject();
            if (data.get("protocol").getAsInt() != PROTOCOL)
                throw new IOException("Bridge protocol mismatch. Install matching Rider and Unreal plugins.");
            return new BridgeClient(URI.create(data.get("endpoint").getAsString()), data.get("token").getAsString());
        } catch (IllegalArgumentException | IllegalStateException | NullPointerException e) {
            throw new IOException("Invalid bridge session file.", e);
        }
    }

    JsonObject open(Path asset) throws IOException {
        JsonObject data = new JsonObject();
        data.addProperty("filename", asset.toAbsolutePath().toString());
        return json("open", data);
    }
    JsonObject details(String session, String component) throws IOException {
        return details(session, component, false);
    }
    JsonObject details(String session, String component, boolean advanced) throws IOException {
        JsonObject data = session(session);
        data.addProperty("component", component);
        data.addProperty("advanced", advanced);
        return json("details", data);
    }
    byte[] frame(String session, double yaw, double pitch, double zoom, String focus) throws IOException {
        JsonObject data = session(session);
        data.addProperty("yaw", yaw); data.addProperty("pitch", pitch); data.addProperty("zoom", zoom);
        data.addProperty("focus", focus);
        return request("frame", data, MAX_IMAGE);
    }
    void close(String session) throws IOException { json("close", session(session)); }
    private static JsonObject session(String session) {
        JsonObject data = new JsonObject(); data.addProperty("session", session); return data;
    }
    private JsonObject json(String route, JsonObject data) throws IOException {
        try {
            return JsonParser.parseString(new String(request(route, data, MAX_JSON), StandardCharsets.UTF_8)).getAsJsonObject();
        } catch (JsonParseException | IllegalStateException e) {
            throw new IOException("The Unreal bridge returned an invalid response.", e);
        }
    }
    private byte[] request(String route, JsonObject data, int limit) throws IOException {
        HttpURLConnection connection = (HttpURLConnection) endpoint.resolve("/rap/v1/" + route).toURL().openConnection(Proxy.NO_PROXY);
        connection.setConnectTimeout(2000); connection.setReadTimeout(10000);
        connection.setInstanceFollowRedirects(false);
        connection.setRequestMethod("POST"); connection.setDoOutput(true);
        connection.setRequestProperty("Authorization", "Bearer " + token);
        connection.setRequestProperty("Content-Type", "application/json; charset=utf-8");
        byte[] body = data.toString().getBytes(StandardCharsets.UTF_8);
        connection.setFixedLengthStreamingMode(body.length);
        try {
            try (OutputStream output = connection.getOutputStream()) { output.write(body); }
            int code = connection.getResponseCode();
            InputStream stream = code == 200 ? connection.getInputStream() : connection.getErrorStream();
            byte[] bytes;
            try (InputStream input = stream) {
                bytes = input == null ? new byte[0] : input.readNBytes((code == 200 ? limit : 8192) + 1);
            }
            if (code != 200) {
                String message = "Unreal bridge returned HTTP " + code + ".";
                try {
                    JsonElement error = JsonParser.parseString(new String(bytes, StandardCharsets.UTF_8));
                    if (error.isJsonObject() && error.getAsJsonObject().has("error"))
                        message = error.getAsJsonObject().get("error").getAsString();
                } catch (RuntimeException ignored) { /* Never show arbitrary server HTML in the IDE. */ }
                throw new IOException(message);
            }
            if (bytes.length > limit) throw new IOException("Unreal bridge response exceeded the size limit.");
            return bytes;
        } finally { connection.disconnect(); }
    }
}
