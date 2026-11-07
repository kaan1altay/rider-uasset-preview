package dev.kaan.uassetpreview;

import com.google.gson.*;

import java.util.*;

/** Immutable comparison domain; independent of Swing, IDE APIs and transport. */
final class Inspection {
    enum Change {
        ADDED("Added"),
        REMOVED("Removed"),
        MODIFIED("Changed"),
        SAME("Unchanged");
        final String label;

        Change(String label) {
            this.label = label;
        }
    }

    record Property(
            String key,
            String category,
            String name,
            String value,
            String canonical,
            String fingerprint,
            String field,
            String tooltip,
            boolean advanced,
            boolean truncated) {}

    record ObjectNode(
            String key,
            String id,
            String name,
            String type,
            String parent,
            String socket,
            Map<String, Property> properties,
            double[] bounds,
            boolean inspected,
            boolean complete) {}

    record Logic(
            String key,
            String graph,
            String name,
            String type,
            String value,
            String fingerprint,
            String tooltip) {}

    record Snapshot(
            String session,
            String name,
            String type,
            String notice,
            boolean visual,
            Map<String, ObjectNode> objects,
            Map<String, Logic> logic,
            double[] bounds,
            boolean limited,
            boolean logicComplete,
            boolean hierarchyComplete) {
        static Snapshot read(JsonObject json) {
            Map<String, String> keysById = new HashMap<>();
            keysById.put("asset", "asset");
            JsonArray components = array(json, "components");
            for (JsonElement item : components) {
                JsonObject c = item.getAsJsonObject();
                keysById.put(
                        text(c, "id"), text(c, "key", text(c, "type") + ":" + text(c, "name")));
            }
            Map<String, ObjectNode> objects = new LinkedHashMap<>();
            JsonObject details =
                    json.has("details") ? json.getAsJsonObject("details") : new JsonObject();
            boolean limited =
                    bool(json, "componentsTruncated")
                            || bool(json, "logicTruncated")
                            || bool(json, "hierarchyTruncated");
            JsonObject root = new JsonObject();
            root.addProperty("id", "asset");
            root.addProperty("name", text(json, "name"));
            root.addProperty("type", text(json, "type"));
            root.add("bounds", json.get("bounds"));
            List<JsonObject> all = new ArrayList<>();
            all.add(root);
            for (JsonElement c : components) all.add(c.getAsJsonObject());
            for (JsonObject c : all) {
                String id = text(c, "id"), key = keysById.getOrDefault(id, "asset");
                Map<String, Property> properties = new LinkedHashMap<>();
                JsonObject values = details.has(id) ? details.getAsJsonObject(id) : null;
                if (values == null || bool(values, "truncated")) limited = true;
                if (values != null)
                    for (JsonElement item : array(values, "properties")) {
                        JsonObject p = item.getAsJsonObject();
                        String pkey =
                                text(
                                        p,
                                        "key",
                                        text(p, "category")
                                                + "/"
                                                + text(p, "property")
                                                + "/"
                                                + text(p, "name"));
                        if (properties.containsKey(pkey)) {
                            limited = true;
                            pkey += "#" + properties.size();
                        }
                        boolean truncated = bool(p, "truncated");
                        limited |= truncated;
                        properties.put(
                                pkey,
                                new Property(
                                        pkey,
                                        text(p, "category"),
                                        text(p, "name"),
                                        text(p, "value"),
                                        text(p, "diffValue", text(p, "value")),
                                        text(
                                                p,
                                                "fingerprint",
                                                text(p, "diffValue", text(p, "value"))),
                                        text(p, "property"),
                                        text(p, "tooltip"),
                                        bool(p, "advanced"),
                                        truncated));
                    }
                if (objects.containsKey(key)) {
                    limited = true;
                    key += ":ambiguous:" + id;
                }
                objects.put(
                        key,
                        new ObjectNode(
                                key,
                                id,
                                text(c, "name"),
                                text(c, "type"),
                                keysById.getOrDefault(text(c, "parent"), "asset"),
                                text(c, "socket"),
                                Map.copyOf(properties),
                                Inspection.bounds(c),
                                values != null,
                                values != null && !bool(values, "truncated")));
            }
            Map<String, Logic> logic = new LinkedHashMap<>();
            for (JsonElement item : array(json, "logic")) {
                JsonObject n = item.getAsJsonObject();
                String key = text(n, "key");
                if (logic.containsKey(key)) {
                    limited = true;
                    key += "#" + logic.size();
                }
                limited |= bool(n, "truncated");
                logic.put(
                        key,
                        new Logic(
                                key,
                                text(n, "graph"),
                                text(n, "name"),
                                text(n, "type"),
                                text(n, "value"),
                                text(n, "fingerprint", text(n, "value")),
                                text(n, "tooltip")));
            }
            return new Snapshot(
                    text(json, "session"),
                    text(json, "name"),
                    text(json, "type"),
                    text(json, "notice"),
                    bool(json, "visual"),
                    Collections.unmodifiableMap(objects),
                    Collections.unmodifiableMap(logic),
                    Inspection.bounds(json),
                    limited,
                    !bool(json, "logicTruncated"),
                    !bool(
                            json,
                            json.has("hierarchyTruncated")
                                    ? "hierarchyTruncated"
                                    : "componentsTruncated"));
        }
    }

    record Row(
            String objectKey,
            String category,
            String name,
            String before,
            String after,
            Change change,
            String explanation,
            boolean advanced,
            boolean logic) {}

    record Component(
            String key, String name, String type, String parent, Change change, long differences) {}

    record Review(
            List<Row> rows,
            List<Component> components,
            int added,
            int removed,
            long settings,
            long logic,
            boolean limited,
            Map<String, String> owners) {
        String owner(Row row) {
            return owners.getOrDefault(row.objectKey, "Blueprint logic");
        }

        List<Row> visible(String component, boolean changesOnly, String query) {
            String needle = query.toLowerCase(Locale.ROOT).strip();
            return rows.stream()
                    .filter(r -> component == null || r.objectKey.equals(component))
                    .filter(r -> !changesOnly || r.change != Change.SAME)
                    .filter(r ->
                            needle.isEmpty()
                                    || (owner(r)
                                                    + " "
                                                    + r.name
                                                    + " "
                                                    + r.category
                                                    + " "
                                                    + r.before
                                                    + " "
                                                    + r.after
                                                    + " "
                                                    + r.explanation)
                                            .toLowerCase(Locale.ROOT)
                                            .contains(needle))
                    .toList();
        }
    }

    static Review compare(Snapshot before, Snapshot after) {
        Map<String, ObjectNode> a = before == null ? Map.of() : before.objects,
                b = after == null ? Map.of() : after.objects;
        List<Row> rows = new ArrayList<>();
        List<Component> components = new ArrayList<>();
        int added = 0, removed = 0;
        for (String key : union(a.keySet(), b.keySet())) {
            ObjectNode left = a.get(key), right = b.get(key), shown = right == null ? left : right;
            if ((left == null && before != null && !before.hierarchyComplete)
                    || (right == null && after != null && !after.hierarchyComplete)) continue;
            Change structural =
                    left == null ? Change.ADDED : right == null ? Change.REMOVED : Change.SAME;
            if (!key.equals("asset")) {
                if (left == null) added++;
                if (right == null) removed++;
            }
            if (left != null && right != null) {
                structural(
                        rows,
                        key,
                        "Name",
                        left.name,
                        right.name,
                        key.equals("asset")
                                ? "The asset’s name changed."
                                : "The component’s name changed. Its stable Blueprint identity is"
                                        + " preserved.");
                structural(
                        rows,
                        key,
                        "Component type",
                        left.type,
                        right.type,
                        "The component class changed. Different component classes provide different"
                                + " capabilities.");
                if (!left.parent.equals(right.parent)) {
                    String oldParent = readableParent(left.parent, a),
                            newParent = readableParent(right.parent, b);
                    if (oldParent.equals(newParent)) newParent += " (different component)";
                    structural(
                            rows,
                            key,
                            "Attached to",
                            oldParent,
                            newParent,
                            "Attached children follow their parent’s position, rotation and"
                                    + " scale.");
                }
                structural(
                        rows,
                        key,
                        "Attachment socket",
                        left.socket,
                        right.socket,
                        "A named attachment point on the parent mesh or skeleton.");
            }
            Map<String, Property> lp = left == null ? Map.of() : left.properties,
                    rp = right == null ? Map.of() : right.properties;
            // An incomplete export must not be mistaken for deleted properties.
            if ((left == null || left.inspected) && (right == null || right.inspected))
                for (String propertyKey : union(lp.keySet(), rp.keySet())) {
                    Property old = lp.get(propertyKey),
                            next = rp.get(propertyKey),
                            p = next == null ? old : next;
                    if ((old == null && left != null && !left.complete)
                            || (next == null && right != null && !right.complete)) continue;
                    Change change =
                            old == null
                                    ? Change.ADDED
                                    : next == null
                                            ? Change.REMOVED
                                            : old.fingerprint.equals(next.fingerprint)
                                                    ? Change.SAME
                                                    : Change.MODIFIED;
                    String oldValue = old == null ? "—" : old.value,
                            nextValue = next == null ? "—" : next.value;
                    if (change == Change.MODIFIED && oldValue.equals(nextValue)) {
                        oldValue = old.canonical;
                        nextValue = next.canonical;
                        if (oldValue.equals(nextValue))
                            nextValue += " … (content beyond this preview changed)";
                    }
                    String meaning = AssetKnowledge.explain(p.field, p.tooltip, p.category);
                    if (p.truncated)
                        meaning +=
                                " This long value is truncated; inspect it in Unreal for the"
                                        + " complete value.";
                    rows.add(
                            new Row(
                                    key,
                                    p.category,
                                    p.name,
                                    oldValue,
                                    nextValue,
                                    change,
                                    meaning,
                                    p.advanced,
                                    false));
                }
            long changes =
                    rows.stream()
                            .filter(r -> r.objectKey.equals(key) && r.change != Change.SAME)
                            .count();
            if (structural == Change.SAME && changes > 0) structural = Change.MODIFIED;
            components.add(
                    new Component(key, shown.name, shown.type, shown.parent, structural, changes));
        }
        Map<String, Logic> al = before == null ? Map.of() : before.logic,
                bl = after == null ? Map.of() : after.logic;
        for (String key : union(al.keySet(), bl.keySet())) {
            Logic old = al.get(key), next = bl.get(key), n = next == null ? old : next;
            if ((old == null && before != null && !before.logicComplete)
                    || (next == null && after != null && !after.logicComplete)) continue;
            Change change =
                    old == null
                            ? Change.ADDED
                            : next == null
                                    ? Change.REMOVED
                                    : old.fingerprint.equals(next.fingerprint)
                                                    && old.type.equals(next.type)
                                                    && old.name.equals(next.name)
                                                    && old.graph.equals(next.graph)
                                            ? Change.SAME
                                            : Change.MODIFIED;
            String explanation =
                    "A Blueprint node represents an operation or event. The values below list its"
                        + " input defaults and outgoing pin connections. Graph layout changes are"
                        + " ignored.";
            if (!n.tooltip.isBlank()) explanation += "\n\n" + n.tooltip;
            String oldValue = old == null ? "—" : old.value,
                    nextValue = next == null ? "—" : next.value;
            if (change == Change.MODIFIED && !oldValue.equals(nextValue)) {
                String oldDetails = oldValue, nextDetails = nextValue;
                oldValue = changedLines(oldDetails, nextDetails);
                nextValue = changedLines(nextDetails, oldDetails);
                explanation +=
                        "\n\nFull node before:\n"
                                + oldDetails
                                + "\n\nFull node after:\n"
                                + nextDetails;
            }
            if (change == Change.MODIFIED && oldValue.equals(nextValue))
                nextValue += " … (node details beyond this preview changed)";
            rows.add(
                    new Row(
                            "logic",
                            "Logic / " + n.graph,
                            n.name,
                            oldValue,
                            nextValue,
                            change,
                            explanation,
                            false,
                            true));
        }
        rows.sort(
                Comparator.comparing((Row r) -> r.change == Change.SAME)
                        .thenComparing(Row::objectKey)
                        .thenComparing(Row::category)
                        .thenComparing(Row::name));
        Map<String, String> owners = new HashMap<>();
        for (Component c : components)
            owners.put(c.key, c.key.equals("asset") ? "Asset settings" : c.name);
        return new Review(
                List.copyOf(rows),
                List.copyOf(components),
                added,
                removed,
                rows.stream().filter(r -> !r.logic && r.change != Change.SAME).count(),
                rows.stream().filter(r -> r.logic && r.change != Change.SAME).count(),
                (before != null && before.limited) || (after != null && after.limited),
                Map.copyOf(owners));
    }

    /** Keep changed pin/default lines prominent without losing repeated lines or full details. */
    static String changedLines(String value, String other) {
        Map<String, Integer> remaining = new HashMap<>();
        other.lines().forEach(line -> remaining.merge(line, 1, Integer::sum));
        List<String> changed = new ArrayList<>();
        value.lines()
                .forEach(line -> {
                    int count = remaining.getOrDefault(line, 0);
                    if (count == 0) changed.add(line);
                    else remaining.put(line, count - 1);
                });
        return changed.isEmpty() ? "No additional pin/default lines" : String.join("\n", changed);
    }

    private static void structural(
            List<Row> rows,
            String key,
            String name,
            String before,
            String after,
            String explanation) {
        if (!before.equals(after))
            rows.add(
                    new Row(
                            key,
                            "Structure",
                            name,
                            before,
                            after,
                            Change.MODIFIED,
                            explanation,
                            false,
                            false));
    }

    private static String readableParent(String key, Map<String, ObjectNode> objects) {
        ObjectNode p = objects.get(key);
        return p == null ? "Asset root" : p.name;
    }

    private static Set<String> union(Set<String> a, Set<String> b) {
        Set<String> all = new LinkedHashSet<>(a);
        all.addAll(b);
        return all;
    }

    static double[] combineBounds(Snapshot a, Snapshot b, String key) {
        double[] result = null;
        for (Snapshot s : new Snapshot[] {a, b}) {
            if (s == null || !s.visual) continue;
            ObjectNode object = s.objects.get(key);
            double[] box = object == null ? s.bounds : object.bounds;
            if (result == null) result = box.clone();
            else
                for (int i = 0; i < 3; i++) {
                    result[i] = Math.min(result[i], box[i]);
                    result[i + 3] = Math.max(result[i + 3], box[i + 3]);
                }
        }
        return result;
    }

    private static double[] bounds(JsonObject j) {
        JsonArray values = array(j, "bounds");
        double[] b = {-50, -50, -50, 50, 50, 50};
        if (values.size() == 6)
            for (int i = 0; i < 6; i++) {
                double v = values.get(i).getAsDouble();
                if (Double.isFinite(v)) b[i] = v;
            }
        return b;
    }

    private static JsonArray array(JsonObject j, String key) {
        return j.has(key) && j.get(key).isJsonArray() ? j.getAsJsonArray(key) : new JsonArray();
    }

    static String text(JsonObject j, String key) {
        return text(j, key, "");
    }

    static String text(JsonObject j, String key, String fallback) {
        return j.has(key) && !j.get(key).isJsonNull() ? j.get(key).getAsString() : fallback;
    }

    private static boolean bool(JsonObject j, String key) {
        return j.has(key) && !j.get(key).isJsonNull() && j.get(key).getAsBoolean();
    }

    private Inspection() {}
}
