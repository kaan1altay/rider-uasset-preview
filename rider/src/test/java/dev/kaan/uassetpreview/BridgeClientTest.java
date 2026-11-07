package dev.kaan.uassetpreview;

import com.google.gson.*;
import com.sun.net.httpserver.*;

import java.awt.image.BufferedImage;
import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.concurrent.atomic.AtomicInteger;

import javax.imageio.ImageIO;

/** Real loopback HTTP tests; the test server stands in for Unreal, not for Rider UI integration. */
public final class BridgeClientTest {
    private static int checks;

    private static void check(boolean value, String message) {
        checks++;
        if (!value) throw new AssertionError(message);
    }

    private static void rejects(Throwing action, String message) throws Exception {
        try {
            action.run();
        } catch (IOException expected) {
            checks++;
            return;
        }
        throw new AssertionError(message);
    }

    @FunctionalInterface
    private interface Throwing {
        void run() throws Exception;
    }

    public static void main(String[] arguments) throws Exception {
        String token = "a".repeat(64);
        rejects(
                () -> new BridgeClient(URI.create("http://example.com:32187"), token),
                "Non-loopback endpoint accepted");
        rejects(
                () -> new BridgeClient(URI.create("http://127.0.0.1:32187/redirect"), token),
                "Endpoint path accepted");
        rejects(
                () -> new BridgeClient(URI.create("http://127.0.0.1:32187"), "secret"),
                "Invalid token accepted");
        HttpServer server = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
        AtomicInteger authenticated = new AtomicInteger(),
                closed = new AtomicInteger(),
                advanced = new AtomicInteger(),
                commonCamera = new AtomicInteger();
        String requestId = "b".repeat(32);
        ByteArrayOutputStream imageBytes = new ByteArrayOutputStream();
        ImageIO.write(new BufferedImage(2, 2, BufferedImage.TYPE_INT_RGB), "png", imageBytes);
        server.createContext(
                "/rap/v1/",
                exchange -> {
                    try {
                        check("POST".equals(exchange.getRequestMethod()), "Expected POST");
                        check(
                                ("Bearer " + token)
                                        .equals(
                                                exchange.getRequestHeaders()
                                                        .getFirst("Authorization")),
                                "Authorization lost");
                        authenticated.incrementAndGet();
                        JsonObject body =
                                JsonParser.parseString(
                                                new String(
                                                        exchange.getRequestBody().readAllBytes(),
                                                        StandardCharsets.UTF_8))
                                        .getAsJsonObject();
                        String route = exchange.getRequestURI().getPath();
                        String response;
                        if (route.endsWith("/open")) {
                            check(
                                    body.get("filename").getAsString().endsWith("BP_Örnek.uasset"),
                                    "Unicode path damaged");
                            response =
                                    "{\"session\":\"s1\",\"name\":\"BP_Örnek\",\"type\":\"Blueprint\",\"components\":[]}";
                        } else if (route.endsWith("/snapshot")) {
                            check(
                                    body.get("filename").getAsString().endsWith("BP_Örnek.uasset"),
                                    "Snapshot original Unicode path lost");
                            check(
                                    body.get("snapshot").getAsString().endsWith("Revision.uasset"),
                                    "Snapshot staging path lost");
                            check(
                                    body.get("request").getAsString().equals(requestId),
                                    "Cleanup request ID missing");
                            response =
                                    "{\"session\":\""
                                            + requestId
                                            + "\",\"components\":[],\"details\":{},\"logic\":[]}";
                        } else if (route.endsWith("/details")) {
                            check(body.has("advanced"), "Details visibility preference missing");
                            if (body.get("advanced").getAsBoolean()) advanced.incrementAndGet();
                            if (body.get("component").getAsString().equals("missing")) {
                                send(
                                        exchange,
                                        404,
                                        "{\"error\":\"Unknown component.\"}"
                                                .getBytes(StandardCharsets.UTF_8));
                                return;
                            }
                            if (body.get("component").getAsString().equals("broken")) {
                                send(
                                        exchange,
                                        200,
                                        "invalid JSON".getBytes(StandardCharsets.UTF_8));
                                return;
                            }
                            check("s1".equals(body.get("session").getAsString()), "Wrong session");
                            response =
                                    "{\"properties\":[{\"category\":\"Transform\",\"name\":\"Location\",\"value\":\"(X=0,Y=0,Z=0)\"}]}";
                        } else if (route.endsWith("/frame")) {
                            check(
                                    body.get("zoom").getAsDouble() == 1.5
                                            && body.get("focus").getAsString().equals("mesh"),
                                    "Camera data damaged");
                            if (body.has("bounds")) {
                                check(
                                        body.getAsJsonArray("bounds").size() == 6,
                                        "Common camera bounds damaged");
                                commonCamera.incrementAndGet();
                            }
                            send(exchange, 200, imageBytes.toByteArray());
                            return;
                        } else {
                            check(
                                    "s1".equals(body.get("session").getAsString())
                                            || requestId.equals(body.get("session").getAsString()),
                                    "Close targeted wrong session");
                            closed.incrementAndGet();
                            response = "{}";
                        }
                        send(exchange, 200, response.getBytes(StandardCharsets.UTF_8));
                    } finally {
                        exchange.close();
                    }
                });
        server.start();
        Path temp = Files.createTempDirectory("uasset-preview-test-");
        try {
            Path project = Files.createDirectories(temp.resolve("project"));
            Path asset = project.resolve("Content/Actors/BP_Örnek.uasset");
            Files.createDirectories(asset.getParent());
            Path descriptor = project.resolve("Saved/RiderAssetPreview/session.json");
            Files.createDirectories(descriptor.getParent());
            String endpoint = "http://127.0.0.1:" + server.getAddress().getPort();
            Files.writeString(
                    descriptor,
                    "{\"protocol\":1,\"endpoint\":\""
                            + endpoint
                            + "\",\"token\":\""
                            + token
                            + "\"}");
            check(
                    BridgeClient.discover(asset, null).equals(descriptor),
                    "Project ancestor discovery failed");
            check(
                    BridgeClient.discover(temp.resolve("engine/EngineAsset.uasset"), project)
                            .equals(descriptor),
                    "External mount discovery failed");
            BridgeClient client = BridgeClient.fromDescriptor(descriptor);
            check(client.open(asset).get("session").getAsString().equals("s1"), "Open result lost");
            check(
                    client.snapshot(asset, temp.resolve("Revision.uasset"), requestId)
                            .get("session")
                            .getAsString()
                            .equals(requestId),
                    "Snapshot result identity lost");
            check(
                    client.details("s1", "mesh").getAsJsonArray("properties").size() == 1,
                    "Properties lost");
            check(advanced.get() == 0, "Advanced rows should be hidden by default");
            check(
                    client.details("s1", "mesh", true).getAsJsonArray("properties").size() == 1,
                    "Advanced details response lost");
            check(advanced.get() == 1, "Advanced details preference not sent");
            check(
                    ImageIO.read(new ByteArrayInputStream(client.frame("s1", 40, 20, 1.5, "mesh")))
                                    .getWidth()
                            == 2,
                    "Frame cannot be decoded");
            check(
                    ImageIO.read(
                                            new ByteArrayInputStream(
                                                    client.frame(
                                                            "s1",
                                                            40,
                                                            20,
                                                            1.5,
                                                            "mesh",
                                                            new double[] {
                                                                -50, -50, -50, 100, 50, 50
                                                            })))
                                    .getWidth()
                            == 2,
                    "Common-camera frame failed");
            check(commonCamera.get() == 1, "Common camera data was not sent");
            try {
                client.details("s1", "missing");
                throw new AssertionError("HTTP error swallowed");
            } catch (IOException error) {
                check(error.getMessage().equals("Unknown component."), "HTTP error not surfaced");
            }
            rejects(() -> client.details("s1", "broken"), "Malformed JSON accepted");
            client.close("s1");
            client.close(requestId);
            check(closed.get() == 2, "Remote sessions not closed");
            check(authenticated.get() == 10, "Not all requests authenticated");
            Files.writeString(descriptor, "{\"protocol\":2}");
            rejects(() -> BridgeClient.fromDescriptor(descriptor), "Protocol mismatch accepted");
            Files.writeString(descriptor, "x".repeat(8193));
            rejects(
                    () -> BridgeClient.fromDescriptor(descriptor),
                    "Oversized session descriptor accepted");
            System.out.println(
                    "PASS: "
                            + checks
                            + " checks; authenticated HTTP open/details/frame/close, Unicode paths,"
                            + " discovery and errors.");
        } finally {
            server.stop(0);
            // Delete only files beneath the unique temp directory created by this test.
            try (var paths = Files.walk(temp)) {
                for (Path path : paths.sorted(java.util.Comparator.reverseOrder()).toList())
                    Files.delete(path);
            }
        }
    }

    private static void send(HttpExchange exchange, int status, byte[] bytes) throws IOException {
        exchange.sendResponseHeaders(status, bytes.length);
        try (OutputStream output = exchange.getResponseBody()) {
            output.write(bytes);
        }
    }
}
