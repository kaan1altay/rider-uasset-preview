package dev.kaan.uassetpreview;

import javax.swing.*;

/** Asset-authored strings are text, never Swing HTML or remote image markup. */
final class UiText {
    static <T extends JComponent> T plain(T component) {
        component.putClientProperty("html.disable", Boolean.TRUE);
        return component;
    }

    static JToolTip tooltip(JComponent owner) {
        JToolTip tip = plain(new JToolTip());
        tip.setComponent(owner);
        return tip;
    }

    private UiText() {}
}
