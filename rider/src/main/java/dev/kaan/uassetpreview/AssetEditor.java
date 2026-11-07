package dev.kaan.uassetpreview;

import com.google.gson.*;
import com.intellij.openapi.fileEditor.*;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.util.UserDataHolderBase;
import com.intellij.openapi.vfs.VirtualFile;
import com.intellij.ui.components.*;
import com.intellij.ui.table.JBTable;
import com.intellij.ui.treeStructure.Tree;

import org.jetbrains.annotations.*;

import java.awt.*;
import java.awt.event.*;
import java.awt.image.BufferedImage;
import java.beans.PropertyChangeListener;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;

import javax.imageio.ImageIO;
import javax.swing.*;
import javax.swing.table.DefaultTableModel;
import javax.swing.table.TableRowSorter;
import javax.swing.tree.*;

final class AssetEditor extends UserDataHolderBase implements FileEditor {
    private final Project project;
    private final VirtualFile file;
    private final JPanel panel = new JPanel(new BorderLayout(0, 8));
    private final JLabel status = new JLabel("Connecting to Unreal Editor…");
    private final JLabel title = new JLabel();
    private final Tree tree = new Tree(new DefaultMutableTreeNode("Components"));
    private final DefaultTableModel properties =
            new DefaultTableModel(new Object[] {"Category", "Property", "Value"}, 0) {
                @Override
                public boolean isCellEditable(int row, int column) {
                    return false;
                }
            };
    private final Viewport viewport = new Viewport();
    private final JButton reconnect = new JButton("Reconnect / reload asset");
    private final JCheckBox advanced = new JCheckBox("Advanced");
    private final JBTextField search = new JBTextField();
    private final JTextArea explanation = new JTextArea(4, 20);
    private final java.util.List<JsonObject> propertyMetadata = new ArrayList<>();
    private boolean visual = true;
    private final ExecutorService worker =
            Executors.newSingleThreadExecutor(task -> {
                Thread thread = new Thread(task, "Unreal asset preview");
                thread.setDaemon(true);
                return thread;
            });
    private final javax.swing.Timer cameraTimer;
    private volatile boolean disposed;
    // Bridge and session are owned by the worker, UI state by the Swing EDT.
    private BridgeClient bridge;
    private String session;
    private int generation;
    private volatile int selectionRevision;
    private String selected = "asset", focus = "asset";
    private double yaw = 40, pitch = 20, zoom = 1;
    private boolean loading, framePending, frameDirty;

    AssetEditor(Project project, VirtualFile file) {
        this.project = project;
        this.file = file;
        UiText.plain(title);
        UiText.plain(status);
        title.setText(file.getName());
        JPanel toolbar = new JPanel(new FlowLayout(FlowLayout.LEADING, 8, 4));
        toolbar.add(title);
        toolbar.add(reconnect);
        JButton frame = new JButton("Frame selected"), reset = new JButton("Frame asset");
        toolbar.add(frame);
        toolbar.add(reset);
        reconnect.addActionListener(event -> reload());
        frame.addActionListener(event -> {
            focus = selected;
            zoom = 1;
            requestFrame();
        });
        reset.addActionListener(event -> {
            focus = "asset";
            zoom = 1;
            yaw = 40;
            pitch = 20;
            requestFrame();
        });
        JBTable table =
                new JBTable(properties) {
                    @Override
                    public JToolTip createToolTip() {
                        return UiText.tooltip(this);
                    }

                    @Override
                    public String getToolTipText(MouseEvent event) {
                        int row = rowAtPoint(event.getPoint()),
                                column = columnAtPoint(event.getPoint());
                        return row < 0 || column < 0
                                ? null
                                : getColumnName(column) + ": " + getValueAt(row, column);
                    }
                };
        table.setDefaultRenderer(
                Object.class, UiText.plain(new javax.swing.table.DefaultTableCellRenderer()));
        tree.setCellRenderer(UiText.plain(new DefaultTreeCellRenderer()));
        table.setAutoResizeMode(JTable.AUTO_RESIZE_LAST_COLUMN);
        table.setRowHeight(Math.max(24, table.getRowHeight()));
        table.setCellSelectionEnabled(true);
        table.getAccessibleContext().setAccessibleName("Read-only component properties");
        TableRowSorter<DefaultTableModel> sorter = new TableRowSorter<>(properties);
        table.setRowSorter(sorter);
        search.getEmptyText().setText("Find a setting or value");
        search.getAccessibleContext().setAccessibleName("Search asset settings");
        search.getDocument()
                .addDocumentListener(
                        new javax.swing.event.DocumentListener() {
                            private void changed() {
                                sorter.setRowFilter(
                                        search.getText().isBlank()
                                                ? null
                                                : RowFilter.regexFilter(
                                                        "(?i)"
                                                                + java.util.regex.Pattern.quote(
                                                                        search.getText())));
                            }

                            public void insertUpdate(javax.swing.event.DocumentEvent e) {
                                changed();
                            }

                            public void removeUpdate(javax.swing.event.DocumentEvent e) {
                                changed();
                            }

                            public void changedUpdate(javax.swing.event.DocumentEvent e) {
                                changed();
                            }
                        });
        explanation.setEditable(false);
        explanation.setLineWrap(true);
        explanation.setWrapStyleWord(true);
        explanation.setFont(UIManager.getFont("Label.font"));
        explanation.setBackground(UIManager.getColor("Panel.background"));
        explanation.setBorder(BorderFactory.createEmptyBorder(8, 8, 8, 8));
        table.getSelectionModel()
                .addListSelectionListener(event -> {
                    int selectedRow = table.getSelectedRow();
                    if (selectedRow < 0) return;
                    int row = table.convertRowIndexToModel(selectedRow);
                    if (row >= propertyMetadata.size()) return;
                    JsonObject p = propertyMetadata.get(row);
                    explanation.setText(
                            Inspection.text(p, "name")
                                    + "\n"
                                    + AssetKnowledge.explain(
                                            Inspection.text(p, "property"),
                                            Inspection.text(p, "tooltip"),
                                            Inspection.text(p, "category")));
                    explanation.setCaretPosition(0);
                });
        tree.getAccessibleContext().setAccessibleName("Asset component hierarchy");
        tree.addTreeSelectionListener(event -> {
            DefaultMutableTreeNode node =
                    (DefaultMutableTreeNode) tree.getLastSelectedPathComponent();
            if (node != null && node.getUserObject() instanceof ComponentNode component)
                select(component.id());
        });
        JPanel settings = new JPanel(new BorderLayout(0, 8));
        settings.add(search, BorderLayout.NORTH);
        settings.add(new JBScrollPane(table), BorderLayout.CENTER);
        settings.add(new JBScrollPane(explanation), BorderLayout.SOUTH);
        JPanel detailsPane = section("Details · read-only", settings);
        JPanel detailsHeading = new JPanel(new BorderLayout());
        detailsHeading.add(new JLabel("Details · read-only"), BorderLayout.CENTER);
        detailsHeading.add(advanced, BorderLayout.EAST);
        detailsPane.add(detailsHeading, BorderLayout.NORTH);
        advanced.addActionListener(event -> select(selected));
        detailsPane.setMinimumSize(new Dimension(230, 160));
        viewport.setMinimumSize(new Dimension(200, 160));
        JPanel hierarchyPane = section("Components", new JBScrollPane(tree));
        hierarchyPane.setMinimumSize(new Dimension(150, 160));
        JSplitPane content = new JSplitPane(JSplitPane.HORIZONTAL_SPLIT, viewport, detailsPane);
        content.setResizeWeight(0.60);
        JSplitPane all = new JSplitPane(JSplitPane.HORIZONTAL_SPLIT, hierarchyPane, content);
        all.setResizeWeight(0.22);
        // File editors are constructed before their final size is known. Pixel offsets collapse
        // the details pane in a narrow editor, so initialize proportions after the first layout.
        panel.addComponentListener(
                new ComponentAdapter() {
                    private boolean initialized;

                    @Override
                    public void componentResized(ComponentEvent event) {
                        if (initialized || panel.getWidth() <= 0 || panel.getHeight() <= 0) return;
                        initialized = true;
                        all.setDividerLocation(0.22);
                        SwingUtilities.invokeLater(() -> content.setDividerLocation(0.60));
                    }
                });
        panel.add(toolbar, BorderLayout.NORTH);
        panel.add(all, BorderLayout.CENTER);
        JPanel footer = new JPanel(new BorderLayout());
        footer.add(status, BorderLayout.CENTER);
        footer.add(new JLabel("Drag to orbit · Scroll to zoom"), BorderLayout.EAST);
        panel.add(footer, BorderLayout.SOUTH);
        panel.setBorder(BorderFactory.createEmptyBorder(8, 8, 8, 8));
        cameraTimer =
                new javax.swing.Timer(
                        100,
                        event -> {
                            if (frameDirty && !framePending && !loading) renderFrame();
                        });
        cameraTimer.start();
        reload();
    }

    private static JPanel section(String heading, JComponent content) {
        JPanel section = new JPanel(new BorderLayout(0, 8));
        JLabel label = new JLabel(heading);
        label.setBorder(BorderFactory.createEmptyBorder(4, 8, 0, 0));
        section.add(label, BorderLayout.NORTH);
        section.add(content, BorderLayout.CENTER);
        return section;
    }

    private void reload() {
        if (disposed || loading) return;
        loading = true;
        generation++;
        int ticket = generation;
        reconnect.setEnabled(false);
        properties.setRowCount(0);
        viewport.image = null;
        viewport.repaint();
        frameDirty = false;
        status.setText("Loading asset through Unreal Editor…");
        worker.execute(() -> {
            try {
                closeSession();
                Path base = project.getBasePath() == null ? null : Path.of(project.getBasePath());
                bridge =
                        BridgeClient.fromDescriptor(
                                BridgeClient.discover(Path.of(file.getPath()), base));
                JsonObject asset = bridge.open(Path.of(file.getPath()));
                session = asset.get("session").getAsString();
                onUi(() -> {
                    if (ticket != generation) return;
                    loading = false;
                    reconnect.setEnabled(true);
                    title.setText(
                            asset.get("name").getAsString()
                                    + " · "
                                    + asset.get("type").getAsString());
                    visual = !asset.has("visual") || asset.get("visual").getAsBoolean();
                    viewport.message =
                            visual
                                    ? ""
                                    : "This asset has no 3D geometry. Explore its saved settings in"
                                          + " Details.";
                    viewport.repaint();
                    explanation.setText(AssetKnowledge.asset(asset.get("type").getAsString()));
                    fillHierarchy(asset.getAsJsonArray("components"));
                    status.setText(asset.get("notice").getAsString());
                    focus = "asset";
                    selected = "asset";
                    zoom = 1;
                    requestFrame();
                });
            } catch (Exception error) {
                onUi(() -> {
                    if (ticket != generation) return;
                    loading = false;
                    reconnect.setEnabled(true);
                    showError(error);
                });
            }
        });
    }

    private void fillHierarchy(JsonArray components) {
        DefaultMutableTreeNode root =
                new DefaultMutableTreeNode(new ComponentNode("asset", file.getName(), "Asset"));
        Map<String, DefaultMutableTreeNode> nodes = new LinkedHashMap<>();
        nodes.put("asset", root);
        for (JsonElement item : components) {
            JsonObject data = item.getAsJsonObject();
            String id = data.get("id").getAsString();
            nodes.put(
                    id,
                    new DefaultMutableTreeNode(
                            new ComponentNode(
                                    id,
                                    data.get("name").getAsString(),
                                    data.get("type").getAsString())));
        }
        for (JsonElement item : components) {
            JsonObject data = item.getAsJsonObject();
            DefaultMutableTreeNode node = nodes.get(data.get("id").getAsString());
            DefaultMutableTreeNode parent =
                    nodes.getOrDefault(data.get("parent").getAsString(), root);
            if (node != root && node != parent && !node.isNodeDescendant(parent)) parent.add(node);
            else if (node != root) root.add(node);
        }
        tree.setModel(new DefaultTreeModel(root));
        for (int row = 0; row < tree.getRowCount(); row++) tree.expandRow(row);
        tree.setSelectionPath(new TreePath(root.getPath()));
    }

    private void select(String component) {
        selected = component;
        properties.setRowCount(0);
        propertyMetadata.clear();
        int revision = ++selectionRevision;
        int ticket = generation;
        boolean showAdvanced = advanced.isSelected();
        worker.execute(() -> {
            try {
                if (session == null || disposed || revision != selectionRevision) return;
                JsonObject data = bridge.details(session, component, showAdvanced);
                onUi(() -> {
                    if (ticket != generation
                            || revision != selectionRevision
                            || !selected.equals(component)) return;
                    properties.setRowCount(0);
                    propertyMetadata.clear();
                    for (JsonElement item : data.getAsJsonArray("properties")) {
                        JsonObject property = item.getAsJsonObject();
                        propertyMetadata.add(property);
                        properties.addRow(
                                new Object[] {
                                    property.get("category").getAsString(),
                                    property.get("name").getAsString(),
                                    property.get("value").getAsString()
                                });
                    }
                });
            } catch (Exception error) {
                onUi(() -> {
                    if (ticket == generation
                            && revision == selectionRevision
                            && selected.equals(component)) showError(error);
                });
            }
        });
    }

    private void requestFrame() {
        if (visual) frameDirty = true;
    }

    private void renderFrame() {
        frameDirty = false;
        framePending = true;
        int ticket = generation;
        double frameYaw = yaw, framePitch = pitch, frameZoom = zoom;
        String frameFocus = focus;
        worker.execute(() -> {
            try {
                if (session == null) return;
                byte[] bytes = bridge.frame(session, frameYaw, framePitch, frameZoom, frameFocus);
                BufferedImage image = ImageIO.read(new ByteArrayInputStream(bytes));
                if (image == null)
                    throw new IOException("Unreal did not return a valid preview image.");
                onUi(() -> {
                    if (ticket == generation) {
                        viewport.image = image;
                        viewport.repaint();
                    }
                });
            } catch (Exception error) {
                onUi(() -> {
                    if (ticket == generation) showError(error);
                });
            } finally {
                onUi(() -> framePending = false);
            }
        });
    }

    private void showError(Exception error) {
        status.setText(
                error.getMessage() == null ? error.getClass().getSimpleName() : error.getMessage());
    }

    private void onUi(Runnable action) {
        SwingUtilities.invokeLater(() -> {
            if (!disposed) action.run();
        });
    }

    private void closeSession() {
        if (bridge != null && session != null) {
            try {
                bridge.close(session);
            } catch (IOException ignored) {
            }
        }
        session = null;
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
    public @NotNull String getName() {
        return "Unreal preview";
    }

    @Override
    public void setState(@NotNull FileEditorState state) {}

    @Override
    public boolean isModified() {
        return false;
    }

    @Override
    public boolean isValid() {
        return file.isValid() && !disposed;
    }

    @Override
    public VirtualFile getFile() {
        return file;
    }

    @Override
    public void addPropertyChangeListener(@NotNull PropertyChangeListener listener) {}

    @Override
    public void removePropertyChangeListener(@NotNull PropertyChangeListener listener) {}

    @Override
    public void dispose() {
        disposed = true;
        cameraTimer.stop();
        // Close after any in-flight open request; do not block the UI or leak the remote preview
        // world.
        worker.execute(this::closeSession);
        worker.shutdown();
    }

    private record ComponentNode(String id, String name, String type) {
        @Override
        public String toString() {
            return name + " · " + type;
        }
    }

    private final class Viewport extends JPanel {
        private BufferedImage image;
        private String message = "";
        private Point last;

        Viewport() {
            setPreferredSize(new Dimension(650, 520));
            setFocusable(true);
            getAccessibleContext().setAccessibleName("Unreal asset viewport");
            MouseAdapter mouse =
                    new MouseAdapter() {
                        @Override
                        public void mousePressed(MouseEvent event) {
                            last = event.getPoint();
                        }

                        @Override
                        public void mouseReleased(MouseEvent event) {
                            last = null;
                        }

                        @Override
                        public void mouseDragged(MouseEvent event) {
                            if (last == null) return;
                            yaw -= (event.getX() - last.x) * 0.5;
                            pitch =
                                    Math.max(
                                            -85,
                                            Math.min(85, pitch + (event.getY() - last.y) * 0.5));
                            last = event.getPoint();
                            requestFrame();
                        }

                        @Override
                        public void mouseWheelMoved(MouseWheelEvent event) {
                            zoom =
                                    Math.max(
                                            0.08,
                                            Math.min(
                                                    20,
                                                    zoom
                                                            * Math.pow(
                                                                    1.15,
                                                                    event
                                                                            .getPreciseWheelRotation())));
                            requestFrame();
                        }
                    };
            addMouseListener(mouse);
            addMouseMotionListener(mouse);
            addMouseWheelListener(mouse);
        }

        @Override
        protected void paintComponent(Graphics graphics) {
            super.paintComponent(graphics);
            if (image == null) {
                if (!message.isEmpty()) {
                    graphics.setColor(UIManager.getColor("Label.foreground"));
                    graphics.drawString("No 3D preview", 16, 30);
                    graphics.drawString("Explore this asset’s settings in Details.", 16, 54);
                }
                return;
            }
            Graphics2D g = (Graphics2D) graphics.create();
            g.setRenderingHint(
                    RenderingHints.KEY_INTERPOLATION, RenderingHints.VALUE_INTERPOLATION_BILINEAR);
            double scale =
                    Math.min(
                            (double) getWidth() / image.getWidth(),
                            (double) getHeight() / image.getHeight());
            int width = (int) (image.getWidth() * scale),
                    height = (int) (image.getHeight() * scale);
            g.drawImage(
                    image,
                    (getWidth() - width) / 2,
                    (getHeight() - height) / 2,
                    width,
                    height,
                    null);
            g.dispose();
        }
    }
}
