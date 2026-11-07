package dev.kaan.uassetpreview;

import com.google.gson.*;
import com.intellij.diff.FrameDiffTool;
import com.intellij.diff.contents.*;
import com.intellij.diff.requests.ContentDiffRequest;
import com.intellij.openapi.project.Project;
import com.intellij.ui.JBColor;
import com.intellij.ui.components.*;
import com.intellij.ui.table.JBTable;
import com.intellij.ui.treeStructure.Tree;

import org.jetbrains.annotations.*;

import java.awt.*;
import java.awt.datatransfer.StringSelection;
import java.awt.event.*;
import java.awt.image.BufferedImage;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.List;
import java.util.concurrent.*;

import javax.imageio.ImageIO;
import javax.swing.*;
import javax.swing.event.*;
import javax.swing.table.*;
import javax.swing.tree.*;

/** A local, read-only revision review. HTTP/session work never blocks the Swing EDT. */
final class AssetDiffViewer implements FrameDiffTool.DiffViewer {
    private static final Color ADDED =
            new JBColor(new Color(28, 119, 87), new Color(117, 200, 173));
    private static final Color REMOVED =
            new JBColor(new Color(173, 68, 62), new Color(231, 147, 134));
    private static final Color CHANGED =
            new JBColor(new Color(145, 99, 23), new Color(214, 179, 106));
    private final Project project;
    private final ContentDiffRequest request;
    private final JPanel panel = new JPanel(new BorderLayout(0, 12));
    private final JLabel summary = new JLabel("Preparing asset review…");
    private final JLabel context =
            new JLabel("Compare saved revisions without changing the project");
    private final JLabel status = new JLabel("Connecting to Unreal Editor…");
    private final JTextArea scope = textArea(2);
    private final JTextArea meaning = textArea(6);
    private final Tree tree = new Tree(new DefaultMutableTreeNode("Review"));
    private final JBTextField search = new JBTextField();
    private final JCheckBox onlyChanges = new JCheckBox("Only changes", true);
    private final JButton refresh = new JButton("Refresh review");
    private final JButton copy = new JButton("Copy review");
    private final JComboBox<String> mode =
            new JComboBox<>(
                    new String[] {"Side by side", "Wipe", "Before", "After", "Visual difference"});
    private final ReviewTableModel model = new ReviewTableModel();
    private final JBTable table =
            new JBTable(model) {
                @Override
                public JToolTip createToolTip() {
                    return UiText.tooltip(this);
                }

                @Override
                public String getToolTipText(MouseEvent e) {
                    int row = rowAtPoint(e.getPoint()), column = columnAtPoint(e.getPoint());
                    return row < 0 || column < 0 ? null : String.valueOf(getValueAt(row, column));
                }
            };
    private final ComparisonCanvas canvas = new ComparisonCanvas();
    private final JSlider wipePosition = new JSlider(0, 100, 50);
    private JSplitPane contentSplit;
    private final ExecutorService worker =
            Executors.newSingleThreadExecutor(task -> {
                Thread thread = new Thread(task, "Unreal asset revision review");
                thread.setDaemon(true);
                return thread;
            });
    private final javax.swing.Timer timer;
    private final List<String> sessions = new ArrayList<>(); // worker-owned
    private final List<Path> stagedDirectories = new ArrayList<>(); // worker-owned
    private BridgeClient bridge;
    private volatile boolean disposed;
    private boolean initialized, loading, framePending, frameDirty;
    private int generation;
    private Inspection.Snapshot before, after;
    private Inspection.Review review;
    private List<Inspection.Row> shownRows = List.of();
    private String selectedKey, focusKey = "asset";
    private double yaw = 40, pitch = 20, zoom = 1;
    private final String beforeTitle, afterTitle;

    AssetDiffViewer(Project project, ContentDiffRequest request) {
        this.project = project;
        this.request = request;
        UiText.plain(summary);
        UiText.plain(context);
        UiText.plain(status);
        beforeTitle = title(request, 0, "Before");
        afterTitle = title(request, 1, "After");
        panel.setBorder(BorderFactory.createEmptyBorder(14, 16, 10, 16));
        summary.setFont(summary.getFont().deriveFont(Font.BOLD, summary.getFont().getSize2D() + 4));
        context.setForeground(JBColor.GRAY);
        JPanel heading = new JPanel(new BorderLayout(12, 6));
        JPanel words = new JPanel(new GridLayout(2, 1, 0, 5));
        words.add(summary);
        words.add(context);
        heading.add(words, BorderLayout.CENTER);
        JPanel actions = new JPanel(new FlowLayout(FlowLayout.TRAILING, 6, 0));
        actions.add(copy);
        actions.add(refresh);
        copy.setEnabled(false);
        copy.addActionListener(e -> copyReview());
        refresh.addActionListener(e -> reload());
        heading.add(actions, BorderLayout.EAST);
        panel.add(heading, BorderLayout.NORTH);

        tree.setRootVisible(false);
        tree.setShowsRootHandles(true);
        tree.setRowHeight(28);
        tree.getAccessibleContext()
                .setAccessibleName("Changed asset components and Blueprint logic");
        tree.setCellRenderer(
                UiText.plain(
                        new DefaultTreeCellRenderer() {
                            @Override
                            public Component getTreeCellRendererComponent(
                                    JTree t,
                                    Object value,
                                    boolean selected,
                                    boolean expanded,
                                    boolean leaf,
                                    int row,
                                    boolean hasFocus) {
                                super.getTreeCellRendererComponent(
                                        t, value, selected, expanded, leaf, row, hasFocus);
                                Object object = ((DefaultMutableTreeNode) value).getUserObject();
                                if (object instanceof Nav n) {
                                    setText(n.label);
                                    if (!selected) setForeground(color(n.change));
                                }
                                return this;
                            }
                        }));
        tree.addTreeSelectionListener(e -> {
            DefaultMutableTreeNode node =
                    (DefaultMutableTreeNode) tree.getLastSelectedPathComponent();
            if (node != null && node.getUserObject() instanceof Nav n) {
                selectedKey = n.key;
                refreshRows();
            }
        });
        JPanel navigator = new JPanel(new BorderLayout(0, 8));
        JLabel navTitle = new JLabel("Change navigator");
        navTitle.setFont(navTitle.getFont().deriveFont(Font.BOLD));
        navigator.add(navTitle, BorderLayout.NORTH);
        navigator.add(new JBScrollPane(tree), BorderLayout.CENTER);
        navigator.setMinimumSize(new Dimension(170, 160));

        JPanel visual = new JPanel(new BorderLayout(0, 6));
        JPanel visualTools = new JPanel(new BorderLayout());
        JPanel controls = new JPanel(new FlowLayout(FlowLayout.LEADING, 6, 0));
        controls.add(mode);
        controls.add(new JLabel("Linked camera"));
        JButton frame = new JButton("Frame asset"), selected = new JButton("Frame selection");
        controls.add(frame);
        controls.add(selected);
        visualTools.add(controls, BorderLayout.WEST);
        wipePosition.setPreferredSize(new Dimension(100, 24));
        wipePosition.setVisible(false);
        wipePosition.getAccessibleContext().setAccessibleName("Before and after wipe boundary");
        wipePosition.setToolTipText("Move the before/after boundary");
        controls.add(wipePosition);
        wipePosition.addChangeListener(e -> {
            canvas.wipe = wipePosition.getValue() / 100.0;
            canvas.repaint();
        });
        frame.addActionListener(e -> {
            focusKey = "asset";
            yaw = 40;
            pitch = 20;
            zoom = 1;
            frameDirty = true;
        });
        selected.addActionListener(e -> {
            focusKey = selectedKey == null || selectedKey.equals("logic") ? "asset" : selectedKey;
            zoom = 1;
            frameDirty = true;
        });
        mode.addActionListener(e -> {
            wipePosition.setVisible(mode.getSelectedIndex() == 1);
            visualTools.revalidate();
            canvas.repaint();
        });
        visual.add(visualTools, BorderLayout.NORTH);
        visual.add(canvas, BorderLayout.CENTER);
        visual.setMinimumSize(new Dimension(300, 160));

        JPanel details = new JPanel(new BorderLayout(0, 8));
        JPanel filters = new JPanel(new BorderLayout(10, 0));
        search.getEmptyText().setText("Find settings, components or values");
        search.getAccessibleContext().setAccessibleName("Search asset changes");
        filters.add(search, BorderLayout.CENTER);
        filters.add(onlyChanges, BorderLayout.EAST);
        search.getDocument()
                .addDocumentListener(
                        new DocumentListener() {
                            public void insertUpdate(DocumentEvent e) {
                                refreshRows();
                            }

                            public void removeUpdate(DocumentEvent e) {
                                refreshRows();
                            }

                            public void changedUpdate(DocumentEvent e) {
                                refreshRows();
                            }
                        });
        onlyChanges.addActionListener(e -> refreshRows());
        details.add(filters, BorderLayout.NORTH);
        table.setRowHeight(Math.max(30, table.getRowHeight()));
        table.setCellSelectionEnabled(true);
        table.setShowGrid(false);
        table.setIntercellSpacing(new Dimension(0, 0));
        table.getAccessibleContext().setAccessibleName("Before and after asset settings");
        table.getColumnModel().getColumn(0).setMaxWidth(95);
        table.getColumnModel().getColumn(1).setPreferredWidth(260);
        table.setDefaultRenderer(
                String.class,
                UiText.plain(
                        new DefaultTableCellRenderer() {
                            @Override
                            public Component getTableCellRendererComponent(
                                    JTable t,
                                    Object value,
                                    boolean selected,
                                    boolean focused,
                                    int row,
                                    int column) {
                                super.getTableCellRendererComponent(
                                        t, value, selected, focused, row, column);
                                setBorder(BorderFactory.createEmptyBorder(4, 8, 4, 8));
                                if (!selected && row < shownRows.size())
                                    setForeground(
                                            column == 0
                                                    ? color(shownRows.get(row).change())
                                                    : t.getForeground());
                                return this;
                            }
                        }));
        table.getSelectionModel()
                .addListSelectionListener(e -> {
                    if (!e.getValueIsAdjusting()) explainSelection();
                });
        details.add(new JBScrollPane(table), BorderLayout.CENTER);
        JPanel explanation = new JPanel(new BorderLayout(0, 6));
        JLabel explanationTitle = new JLabel("What this means");
        explanationTitle.setFont(explanationTitle.getFont().deriveFont(Font.BOLD));
        explanation.add(explanationTitle, BorderLayout.NORTH);
        explanation.add(new JBScrollPane(meaning), BorderLayout.CENTER);
        explanation.setBorder(BorderFactory.createEmptyBorder(6, 8, 4, 8));
        details.add(explanation, BorderLayout.SOUTH);
        details.setMinimumSize(new Dimension(300, 180));
        JSplitPane right = new JSplitPane(JSplitPane.VERTICAL_SPLIT, visual, details);
        right.setResizeWeight(.46);
        right.setBorder(null);
        contentSplit = right;
        JSplitPane split = new JSplitPane(JSplitPane.HORIZONTAL_SPLIT, navigator, right);
        split.setResizeWeight(.23);
        split.setBorder(null);
        panel.add(split, BorderLayout.CENTER);
        JPanel footer = new JPanel(new BorderLayout(0, 6));
        scope.setForeground(JBColor.GRAY);
        scope.setText(
                "Saved revisions · Dependencies come from the open project · No asset changes are"
                        + " written");
        footer.add(scope, BorderLayout.CENTER);
        footer.add(status, BorderLayout.SOUTH);
        panel.add(footer, BorderLayout.SOUTH);
        panel.addComponentListener(
                new ComponentAdapter() {
                    private boolean set;

                    @Override
                    public void componentResized(ComponentEvent e) {
                        if (set || panel.getWidth() < 1) return;
                        set = true;
                        split.setDividerLocation(.23);
                        SwingUtilities.invokeLater(() -> right.setDividerLocation(.46));
                    }
                });
        panel.getInputMap(JComponent.WHEN_ANCESTOR_OF_FOCUSED_COMPONENT)
                .put(KeyStroke.getKeyStroke("control F"), "search");
        panel.getActionMap()
                .put(
                        "search",
                        new AbstractAction() {
                            @Override
                            public void actionPerformed(ActionEvent e) {
                                search.requestFocusInWindow();
                            }
                        });
        panel.getInputMap(JComponent.WHEN_ANCESTOR_OF_FOCUSED_COMPONENT)
                .put(KeyStroke.getKeyStroke("F7"), "nextChange");
        panel.getActionMap()
                .put(
                        "nextChange",
                        new AbstractAction() {
                            @Override
                            public void actionPerformed(ActionEvent e) {
                                if (shownRows.isEmpty()) return;
                                int row = (table.getSelectedRow() + 1) % shownRows.size();
                                table.setRowSelectionInterval(row, row);
                                table.scrollRectToVisible(table.getCellRect(row, 0, true));
                            }
                        });
        timer =
                new javax.swing.Timer(
                        120,
                        e -> {
                            if (frameDirty && !framePending && !loading) renderFrames();
                        });
    }

    @Override
    public @NotNull JComponent getComponent() {
        return panel;
    }

    @Override
    public @Nullable JComponent getPreferredFocusedComponent() {
        return tree;
    }

    @Override
    public @NotNull FrameDiffTool.ToolbarComponents init() {
        if (!initialized) {
            initialized = true;
            timer.start();
            reload();
        }
        return new FrameDiffTool.ToolbarComponents();
    }

    @Override
    public void dispose() {
        if (disposed) return;
        disposed = true;
        timer.stop();
        worker.execute(this::release);
        worker.shutdown();
    }

    private void reload() {
        if (disposed || loading) return;
        loading = true;
        int ticket = ++generation;
        refresh.setEnabled(false);
        copy.setEnabled(false);
        summary.setText("Loading saved asset revisions…");
        status.setText("Reading before and after from Rider’s diff contents…");
        canvas.left = null;
        canvas.right = null;
        canvas.message = "Loading revisions through Unreal Editor…";
        canvas.repaint();
        before = null;
        after = null;
        review = null;
        model.replace(List.of(), null);
        tree.setEnabled(false);
        worker.execute(() -> {
            try {
                release();
                Path base =
                        project == null || project.getBasePath() == null
                                ? null
                                : Path.of(project.getBasePath());
                List<DiffContent> contents = request.getContents();
                Path anchor = null;
                for (DiffContent c : contents) {
                    Path p = AssetDiffTool.originalPath(c);
                    if (p != null) {
                        anchor = p;
                        break;
                    }
                }
                if (anchor == null)
                    throw new IOException(
                            "Rider did not provide the original asset path. Open this diff from the"
                                    + " project’s Commit or Git History view.");
                Path descriptor = BridgeClient.discover(anchor, base);
                JsonObject metadata =
                        JsonParser.parseString(Files.readString(descriptor)).getAsJsonObject();
                if (!metadata.has("inspectionVersion")
                        || metadata.get("inspectionVersion").getAsInt() < 2)
                    throw new IOException(
                            "The Unreal companion is older than this review feature. Install the"
                                    + " matching Unreal plugin, restart the Editor, then refresh.");
                bridge = BridgeClient.fromDescriptor(descriptor);
                Inspection.Snapshot[] snapshots = new Inspection.Snapshot[2];
                for (int i = 0; i < 2; i++) {
                    if (disposed) return;
                    DiffContent content = contents.get(i);
                    if (content instanceof EmptyContent) continue;
                    Path original = AssetDiffTool.originalPath(content);
                    if (original == null) original = anchor;
                    byte[] bytes =
                            AssetDiffTool.bytes(
                                    content, base == null ? original.getParent() : base);
                    Path stage =
                            descriptor
                                    .getParent()
                                    .resolve("Diff")
                                    .resolve(UUID.randomUUID().toString().replace("-", ""));
                    Files.createDirectories(stage);
                    stagedDirectories.add(stage);
                    Path file = stage.resolve(original.getFileName().toString());
                    Files.write(file, bytes, StandardOpenOption.CREATE_NEW);
                    String requestId = stage.getFileName().toString();
                    sessions.add(requestId);
                    JsonObject data = bridge.snapshot(original, file, requestId);
                    snapshots[i] = Inspection.Snapshot.read(data);
                }
                Inspection.Review result = Inspection.compare(snapshots[0], snapshots[1]);
                ui(() -> {
                    if (ticket != generation) return;
                    before = snapshots[0];
                    after = snapshots[1];
                    review = result;
                    loading = false;
                    refresh.setEnabled(true);
                    copy.setEnabled(true);
                    Inspection.Snapshot shown = after == null ? before : after;
                    summary.setText(
                            result.settings()
                                    + " settings differ   ·   +"
                                    + result.added()
                                    + " / −"
                                    + result.removed()
                                    + " components   ·   "
                                    + result.logic()
                                    + (result.logic() == 1 ? " logic node" : " logic nodes"));
                    context.setText(
                            shown.name()
                                    + "  ·  "
                                    + shown.type()
                                    + "  ·  "
                                    + (before == null
                                            ? "Asset added"
                                            : after == null ? "Asset removed" : "Before → After"));
                    scope.setText(
                            AssetKnowledge.asset(shown.type())
                                    + "\n"
                                    + (result.limited()
                                            ? "Some fields or long descriptions are shortened or"
                                                    + " omitted. "
                                            : "")
                                    + "Saved defaults and Blueprint pin connections only."
                                    + " Dependencies use the open project’s current versions; mesh"
                                    + " geometry and texture pixels are not a semantic diff.");
                    status.setText(
                            "Compared locally · Drag to orbit · Scroll to zoom · F7 for next row");
                    tree.setEnabled(true);
                    canvas.message = "";
                    focusKey = "asset";
                    selectedKey = null;
                    zoom = 1;
                    fillTree();
                    refreshRows();
                    frameDirty = true;
                    contentSplit.setDividerLocation(
                            (before != null && before.visual()) || (after != null && after.visual())
                                    ? .46
                                    : .16);
                });
            } catch (Exception error) {
                ui(() -> {
                    if (ticket != generation) return;
                    loading = false;
                    refresh.setEnabled(true);
                    summary.setText("Asset review needs attention");
                    status.setText(message(error));
                    canvas.message = message(error);
                    canvas.repaint();
                    meaning.setText(
                            "Open the same project in Unreal Editor, confirm the companion plugin"
                                    + " is enabled, and refresh.\n\n"
                                    + message(error));
                });
                release();
            }
        });
    }

    private void fillTree() {
        DefaultMutableTreeNode root = new DefaultMutableTreeNode("Review");
        DefaultMutableTreeNode overview =
                new DefaultMutableTreeNode(new Nav(null, "All changes", Inspection.Change.SAME));
        root.add(overview);
        Map<String, DefaultMutableTreeNode> nodes = new LinkedHashMap<>();
        for (Inspection.Component c : review.components()) {
            String prefix =
                    switch (c.change()) {
                        case ADDED -> "+ ";
                        case REMOVED -> "− ";
                        case MODIFIED -> "~ ";
                        case SAME -> "";
                    };
            String label = c.key().equals("asset") ? "Asset settings" : c.name();
            if (c.differences() > 0) label += "  (" + c.differences() + ")";
            nodes.put(
                    c.key(),
                    new DefaultMutableTreeNode(new Nav(c.key(), prefix + label, c.change())));
        }
        for (Inspection.Component c : review.components()) {
            DefaultMutableTreeNode node = nodes.get(c.key());
            DefaultMutableTreeNode parent =
                    c.key().equals("asset") ? root : nodes.getOrDefault(c.parent(), root);
            if (parent == node || node.isNodeDescendant(parent)) parent = root;
            parent.add(node);
        }
        root.add(
                new DefaultMutableTreeNode(
                        new Nav(
                                "logic",
                                "Blueprint logic  (" + review.logic() + ")",
                                review.logic() > 0
                                        ? Inspection.Change.MODIFIED
                                        : Inspection.Change.SAME)));
        tree.setModel(new DefaultTreeModel(root));
        for (int row = 0; row < tree.getRowCount(); row++) tree.expandRow(row);
        tree.setSelectionPath(new TreePath(overview.getPath()));
    }

    private void refreshRows() {
        if (review == null) return;
        shownRows = review.visible(selectedKey, onlyChanges.isSelected(), search.getText());
        model.replace(shownRows, review);
        if (!shownRows.isEmpty()) table.setRowSelectionInterval(0, 0);
        else
            meaning.setText(
                    onlyChanges.isSelected()
                            ? "No differences in the inspected settings match this selection.\n\n"
                                  + "If the file’s bytes changed, the change may be in hidden"
                                  + " properties, bulk asset data, graph metadata, or other fields"
                                  + " outside this inspection. Turn off “Only changes” to explore"
                                  + " the saved settings."
                            : "No inspected settings match this search or selection.");
    }

    private void explainSelection() {
        int row = table.getSelectedRow();
        if (row < 0 || row >= shownRows.size()) return;
        Inspection.Row r = shownRows.get(row);
        meaning.setText(
                review.owner(r)
                        + " · "
                        + r.name()
                        + " · "
                        + r.change().label
                        + "\n"
                        + "Before: "
                        + r.before()
                        + "\nAfter: "
                        + r.after()
                        + "\n\n"
                        + r.explanation());
        meaning.setCaretPosition(0);
    }

    private void renderFrames() {
        frameDirty = false;
        framePending = true;
        int ticket = generation;
        Inspection.Snapshot left = before, right = after;
        double y = yaw, p = pitch, z = zoom;
        String focus = focusKey;
        double[] bounds = Inspection.combineBounds(left, right, focus);
        worker.execute(() -> {
            try {
                BufferedImage[] images = new BufferedImage[2];
                int i = 0;
                for (Inspection.Snapshot s : new Inspection.Snapshot[] {left, right}) {
                    if (s != null && s.visual()) {
                        Inspection.ObjectNode object = s.objects().get(focus);
                        String id = object == null ? "asset" : object.id();
                        images[i] =
                                ImageIO.read(
                                        new ByteArrayInputStream(
                                                bridge.frame(s.session(), y, p, z, id, bounds)));
                        if (images[i] == null)
                            throw new IOException("Unreal returned an unreadable revision image.");
                    }
                    i++;
                }
                BufferedImage difference = RevisionImages.difference(images[0], images[1]);
                ui(() -> {
                    if (ticket == generation) {
                        canvas.left = images[0];
                        canvas.right = images[1];
                        canvas.difference = difference;
                        canvas.message = "";
                        canvas.repaint();
                        status.setText(
                                "Compared locally · Drag to orbit · Scroll to zoom · F7 for next"
                                        + " row");
                    }
                });
            } catch (Exception error) {
                ui(() -> {
                    if (ticket == generation) {
                        status.setText(message(error));
                        canvas.message = message(error);
                        canvas.repaint();
                    }
                });
            } finally {
                ui(() -> framePending = false);
            }
        });
    }

    private void release() {
        boolean closed = true;
        if (bridge != null)
            for (String session : sessions)
                try {
                    bridge.close(session);
                } catch (IOException ignored) {
                    closed = false;
                }
        sessions.clear();
        bridge = null;
        // If closure could not be acknowledged, retain the bytes: the editor may
        // still be loading them. Saved/ is a disposable local cache, never Git data.
        if (closed)
            for (Path stage : stagedDirectories)
                try {
                    // Only our unique flat staging folder is removed; never follow symbolic links.
                    if (stage.getFileName().toString().matches("[a-f0-9]{32}")) {
                        try (var paths = Files.list(stage)) {
                            for (Path file : paths.toList()) Files.deleteIfExists(file);
                        }
                        Files.deleteIfExists(stage);
                    }
                } catch (IOException ignored) {
                }
        stagedDirectories.clear();
    }

    private void copyReview() {
        if (review == null) return;
        StringBuilder text =
                new StringBuilder("# Unreal asset review\n\n")
                        .append(beforeTitle)
                        .append(" → ")
                        .append(afterTitle)
                        .append("\n\n")
                        .append(review.settings())
                        .append(" settings differ; ")
                        .append(review.added())
                        .append(" components added; ")
                        .append(review.removed())
                        .append(" removed; ")
                        .append(review.logic())
                        .append(" logic nodes differ.\n\n");
        for (Inspection.Row row : review.rows())
            if (row.change() != Inspection.Change.SAME)
                text.append("- ")
                        .append(row.change().label)
                        .append(": ")
                        .append(review.owner(row))
                        .append(" / ")
                        .append(row.category())
                        .append(" / ")
                        .append(row.name())
                        .append("\n  Before: ")
                        .append(row.before().replace("\n", " "))
                        .append("\n  After: ")
                        .append(row.after().replace("\n", " "))
                        .append('\n');
        text.append(
                "\n"
                        + "Scope: editor-visible saved defaults and Blueprint pin connections."
                        + " Dependencies use the current open project. This is not a complete"
                        + " binary/geometry/pixel comparison.");
        if (review.limited()) text.append(" Inspection limits were reached.");
        Toolkit.getDefaultToolkit()
                .getSystemClipboard()
                .setContents(new StringSelection(text.toString()), null);
        status.setText("Review copied to clipboard");
    }

    private void ui(Runnable action) {
        SwingUtilities.invokeLater(() -> {
            if (!disposed) action.run();
        });
    }

    private static String message(Exception error) {
        return error.getMessage() == null ? error.getClass().getSimpleName() : error.getMessage();
    }

    private static String title(ContentDiffRequest r, int side, String fallback) {
        return r.getContentTitles().size() > side && r.getContentTitles().get(side) != null
                ? r.getContentTitles().get(side)
                : fallback;
    }

    private static JTextArea textArea(int rows) {
        JTextArea area = new JTextArea(rows, 20);
        area.setEditable(false);
        area.setLineWrap(true);
        area.setWrapStyleWord(true);
        area.setFont(UIManager.getFont("Label.font"));
        area.setBackground(UIManager.getColor("Panel.background"));
        area.setBorder(BorderFactory.createEmptyBorder(4, 0, 4, 0));
        return area;
    }

    private static Color color(Inspection.Change c) {
        return switch (c) {
            case ADDED -> ADDED;
            case REMOVED -> REMOVED;
            case MODIFIED -> CHANGED;
            case SAME -> UIManager.getColor("Label.foreground");
        };
    }

    private record Nav(String key, String label, Inspection.Change change) {}

    private final class ComparisonCanvas extends JPanel {
        private BufferedImage left, right, difference;
        private String message = "Loading revisions…";
        private Point last;
        private double wipe = .5;

        ComparisonCanvas() {
            setPreferredSize(new Dimension(800, 300));
            getAccessibleContext().setAccessibleName("Linked before and after asset viewports");
            MouseAdapter mouse =
                    new MouseAdapter() {
                        @Override
                        public void mousePressed(MouseEvent e) {
                            last = e.getPoint();
                            if (mode.getSelectedIndex() == 1 && e.isShiftDown()) {
                                wipePosition.setValue(
                                        (int) (100.0 * e.getX() / Math.max(1, getWidth())));
                            }
                        }

                        @Override
                        public void mouseReleased(MouseEvent e) {
                            last = null;
                        }

                        @Override
                        public void mouseDragged(MouseEvent e) {
                            if (last == null) return;
                            if (mode.getSelectedIndex() == 1 && e.isShiftDown()) {
                                wipePosition.setValue(
                                        (int) (100.0 * e.getX() / Math.max(1, getWidth())));
                            } else {
                                yaw -= (e.getX() - last.x) * .5;
                                pitch =
                                        Math.max(
                                                -85,
                                                Math.min(85, pitch + (e.getY() - last.y) * .5));
                                frameDirty = true;
                            }
                            last = e.getPoint();
                        }

                        @Override
                        public void mouseWheelMoved(MouseWheelEvent e) {
                            zoom =
                                    Math.max(
                                            .08,
                                            Math.min(
                                                    20,
                                                    zoom
                                                            * Math.pow(
                                                                    1.15,
                                                                    e.getPreciseWheelRotation())));
                            frameDirty = true;
                        }
                    };
            addMouseListener(mouse);
            addMouseMotionListener(mouse);
            addMouseWheelListener(mouse);
        }

        @Override
        protected void paintComponent(Graphics graphics) {
            super.paintComponent(graphics);
            Graphics2D g = (Graphics2D) graphics.create();
            g.setRenderingHint(
                    RenderingHints.KEY_INTERPOLATION, RenderingHints.VALUE_INTERPOLATION_BILINEAR);
            int w = getWidth(), h = getHeight(), selected = mode.getSelectedIndex();
            if (!message.isEmpty()) {
                g.setColor(JBColor.GRAY);
                drawWrapped(g, message, 20, h / 2, w - 40);
                g.dispose();
                return;
            }
            if (selected == 0) {
                drawSide(g, left, 0, 0, w / 2 - 3, h, "Before · " + beforeTitle, before);
                drawSide(g, right, w / 2 + 3, 0, w - w / 2 - 3, h, "After · " + afterTitle, after);
            } else if (selected == 1) {
                drawSide(g, right, 0, 0, w, h, "", after);
                Shape clip = g.getClip();
                g.clipRect(0, 0, (int) (w * wipe), h);
                drawSide(g, left, 0, 0, w, h, "", before);
                g.setClip(clip);
                g.setColor(UIManager.getColor("Label.foreground"));
                g.drawString("Before", 10, 17);
                g.drawString(
                        "After",
                        Math.max(10, w - g.getFontMetrics().stringWidth("After") - 10),
                        17);
                g.setColor(CHANGED);
                g.drawLine((int) (w * wipe), 0, (int) (w * wipe), h);
                g.setColor(JBColor.GRAY);
                g.drawString("Shift + drag to move the wipe", 12, h - 10);
            } else if (selected == 2)
                drawSide(g, left, 0, 0, w, h, "Before · " + beforeTitle, before);
            else if (selected == 3) drawSide(g, right, 0, 0, w, h, "After · " + afterTitle, after);
            else {
                if (difference != null) {
                    drawSide(
                            g,
                            difference,
                            0,
                            0,
                            w,
                            h,
                            "Pixel differences at shared resolution · lighting and current"
                                    + " dependencies may contribute",
                            after);
                } else drawSide(g, null, 0, 0, w, h, "Visual difference", null);
            }
            g.dispose();
        }

        private void drawSide(
                Graphics2D g,
                BufferedImage image,
                int x,
                int y,
                int w,
                int h,
                String label,
                Inspection.Snapshot snapshot) {
            Shape oldClip = g.getClip();
            g.clipRect(x, y, w, h);
            g.setColor(UIManager.getColor("Label.foreground"));
            g.drawString(label, x + 10, y + 17);
            if (image != null) {
                double scale =
                        Math.min(
                                (double) w / image.getWidth(),
                                (double) (h - 30) / image.getHeight());
                int iw = (int) (image.getWidth() * scale), ih = (int) (image.getHeight() * scale);
                g.drawImage(image, x + (w - iw) / 2, y + 26 + (h - 30 - ih) / 2, iw, ih, null);
            } else {
                g.setColor(JBColor.GRAY);
                String empty =
                        snapshot == null
                                ? "No asset in this revision"
                                : snapshot.visual()
                                        ? "Rendering…"
                                        : "No 3D geometry · inspect the settings below";
                drawWrapped(g, empty, x + 16, y + h / 2, w - 32);
            }
            g.setClip(oldClip);
        }

        private void drawWrapped(Graphics2D g, String text, int x, int y, int width) {
            StringBuilder line = new StringBuilder();
            int baseline = y;
            for (String word : text.split("\\s+")) {
                if (g.getFontMetrics().stringWidth(line + " " + word) > width && !line.isEmpty()) {
                    g.drawString(line.toString(), x, baseline);
                    baseline += g.getFontMetrics().getHeight();
                    line.setLength(0);
                }
                if (!line.isEmpty()) line.append(' ');
                line.append(word);
            }
            g.drawString(line.toString(), x, baseline);
        }
    }
}
