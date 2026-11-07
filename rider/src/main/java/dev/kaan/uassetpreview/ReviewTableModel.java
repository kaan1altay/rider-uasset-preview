package dev.kaan.uassetpreview;

import java.util.List;

import javax.swing.table.AbstractTableModel;

/** One change notification per filter update, even for thousands of settings. */
final class ReviewTableModel extends AbstractTableModel {
    private List<Inspection.Row> rows = List.of();
    private Inspection.Review review;
    private static final String[] COLUMNS = {"Change", "Setting", "Before", "After"};

    void replace(List<Inspection.Row> next, Inspection.Review source) {
        rows = List.copyOf(next);
        review = source;
        fireTableDataChanged();
    }

    @Override
    public int getRowCount() {
        return rows.size();
    }

    @Override
    public int getColumnCount() {
        return COLUMNS.length;
    }

    @Override
    public String getColumnName(int column) {
        return COLUMNS[column];
    }

    @Override
    public Class<?> getColumnClass(int column) {
        return String.class;
    }

    @Override
    public Object getValueAt(int row, int column) {
        Inspection.Row r = rows.get(row);
        return switch (column) {
            case 0 -> r.change().label;
            case 1 -> review.owner(r) + " · " + r.category() + " › " + r.name();
            case 2 -> r.before();
            case 3 -> r.after();
            default -> throw new IndexOutOfBoundsException();
        };
    }
}
