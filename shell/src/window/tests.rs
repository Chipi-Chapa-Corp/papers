use super::*;
use crate::tests::{fixture, wait_until};

#[test]
fn document_menu_moves_and_detaches_its_target_page() {
    crate::tests::run(|| {
        let window = PpsWindow::new();
        window.open(&fixture("utf16le-annot.pdf"), None, None);
        let first = window.imp().current_tab().unwrap();
        window.open(&fixture("ink_annotations.pdf"), None, None);
        let second = window.imp().current_tab().unwrap();
        window.present();
        wait_until("documents", || {
            first.document().is_some() && second.document().is_some()
        });
        let document = first.document();
        let view = window.imp().tabs.get();
        let page = view.page(&first);
        view.emit_by_name::<()>("setup-menu", &[&Some(page.clone())]);
        assert!(
            !window
                .lookup_action("tab-move-backward")
                .unwrap()
                .is_enabled()
        );
        WidgetExt::activate_action(&window, "win.tab-move-forward", None).unwrap();
        assert_eq!(view.nth_page(1), page);
        assert_eq!(window.imp().current_tab(), Some(second));
        view.emit_by_name::<()>("setup-menu", &[&Some(page.clone())]);
        WidgetExt::activate_action(&window, "win.tab-detach", None).unwrap();
        let destination = first.root().and_downcast::<PpsWindow>().unwrap();
        assert_ne!(destination, window);
        assert_eq!(first.document(), document);
        assert_eq!(view.n_pages(), 1);
        assert_eq!(destination.imp().tabs.n_pages(), 1);
        window.close();
        destination.close();
    });
}

#[test]
fn window_actions_follow_the_document_lifecycle() {
    crate::tests::run(|| {
        let window = PpsWindow::new();
        window.open(&fixture("utf16le-annot.pdf"), None, None);
        window.present();
        wait_until("window document", || window.document().is_some());
        assert_eq!(window.title().as_deref(), Some("utf16le-annot.pdf"));

        WidgetExt::activate_action(&window, "win.presentation", None).unwrap();
        assert!(window.imp().current_tab().unwrap().is_presenting());
        WidgetExt::activate_action(&window, "win.escape", None).unwrap();
        assert!(!window.imp().current_tab().unwrap().is_presenting());
        assert!(window.lookup_action("fullscreen").unwrap().is_enabled());

        window.close();
        assert!(!window.is_visible());
    });
}

#[test]
fn repeated_open_requests_keep_the_document_and_apply_the_requested_mode() {
    crate::tests::run(|| {
        let window = PpsWindow::new();
        let file = fixture("utf16le-annot.pdf");
        window.open(&file, None, None);
        window.open(&file, None, Some(WindowRunMode::Fullscreen));
        window.present();
        wait_until("fullscreen document", || window.document().is_some());
        assert!(
            window
                .action_state("fullscreen")
                .unwrap()
                .get::<bool>()
                .unwrap()
        );

        let document = window.document();
        window.open(&file, None, Some(WindowRunMode::Fullscreen));
        assert_eq!(window.document(), document);
        assert_eq!(window.imp().tabs.n_pages(), 1);
        assert!(
            window
                .action_state("fullscreen")
                .unwrap()
                .get::<bool>()
                .unwrap()
        );
        window.close();
    });
}

#[test]
fn file_drops_and_tab_actions_use_the_same_document_pages() {
    crate::tests::run(|| {
        let window = PpsWindow::new();
        window.present();
        WidgetExt::activate_action(&window, "win.new-tab", None).unwrap();
        assert_eq!(window.imp().tabs.n_pages(), 1);
        let target = window
            .observe_controllers()
            .iter::<glib::Object>()
            .flatten()
            .find_map(|controller| controller.downcast::<gtk::DropTarget>().ok())
            .unwrap();
        let files = gdk::FileList::from_array(&[
            fixture("utf16le-annot.pdf"),
            fixture("ink_annotations.pdf"),
            fixture("utf16le-annot.pdf"),
        ]);
        assert!(target.emit_by_name::<bool>(
            "drop",
            &[&glib::BoxedValue(files.to_value()), &0.0f64, &0.0f64],
        ));
        assert_eq!(window.imp().tabs.n_pages(), 2);
        wait_until("dropped documents", || {
            window
                .imp()
                .tabs
                .pages()
                .iter::<adw::TabPage>()
                .all(|page| {
                    page.unwrap()
                        .child()
                        .downcast::<PpsDocumentTab>()
                        .unwrap()
                        .document()
                        .is_some()
                })
        });
        assert_eq!(window.title().as_deref(), Some("utf16le-annot.pdf"));

        WidgetExt::activate_action(&window, "win.new-tab", None).unwrap();
        assert_eq!(window.imp().tabs.n_pages(), 3);
        WidgetExt::activate_action(&window, "win.close", None).unwrap();
        assert_eq!(window.imp().tabs.n_pages(), 2);
        WidgetExt::activate_action(&window, "win.close-window", None).unwrap();
        wait_until("window close", || !window.is_visible());
    });
}

#[test]
fn vertical_tabs_share_selection_and_adapt_to_the_window_width() {
    crate::tests::run(|| {
        let window = PpsWindow::new();
        window
            .imp()
            .default_settings
            .set_int("window-width", 1100)
            .unwrap();
        window.set_default_size(1100, 700);
        window.open(&fixture("utf16le-annot.pdf"), None, None);
        window.open(&fixture("ink_annotations.pdf"), None, None);
        window.present();
        WidgetExt::activate_action(&window, "win.vertical-tabs", None).unwrap();
        wait_until("vertical tabs", || window.imp().tab_sidebar.is_visible());
        assert!(!window.imp().tab_bar.is_visible());
        let pages = window.imp().tab_list.model().unwrap();
        assert_eq!(pages, window.imp().tabs.pages());
        pages.select_item(0, true);
        let first = window.imp().current_tab().unwrap();
        wait_until("selected vertical document", || first.document().is_some());
        assert_eq!(window.title().as_deref(), Some("utf16le-annot.pdf"));

        window.set_default_size(600, 700);
        wait_until("narrow tab layout", || {
            !window.imp().tab_sidebar.is_visible() && window.imp().tab_bar.is_visible()
        });
        assert!(window.imp().settings.boolean("vertical-tabs"));
        assert_eq!(window.imp().current_tab(), Some(first));
        window.set_default_size(1100, 700);
        wait_until("restored vertical tabs", || {
            window.imp().tab_sidebar.is_visible()
        });

        let close = crate::tests::find_widget_where::<gtk::Button>(
            &window.imp().tab_list.get(),
            &|button| button.icon_name().as_deref() == Some("window-close-symbolic"),
        )
        .unwrap();
        close.emit_clicked();
        assert_eq!(window.imp().tabs.n_pages(), 1);
        window
            .imp()
            .settings
            .set_boolean("vertical-tabs", false)
            .unwrap();
        window.close();
        wait_until("window close", || !window.is_visible());
    });
}

#[test]
fn layout_changes_do_not_notify_closed_windows() {
    crate::tests::run(|| {
        let settings = gio::Settings::new("org.gnome.Papers");
        for _ in 0..3 {
            let window = PpsWindow::new();
            window.new_tab();
            window.present();
            // A surviving child may still observe the window's action group.
            let tab_bar = window.imp().tab_bar.get();
            let weak = window.downgrade();
            window.close();
            drop(window);
            wait_until("closed window release", || weak.upgrade().is_none());
            settings
                .set_boolean("vertical-tabs", !settings.boolean("vertical-tabs"))
                .unwrap();
            drop(tab_bar);
        }
        settings.set_boolean("vertical-tabs", false).unwrap();
    });
}

#[test]
fn tabs_reorder_transfer_and_close_through_their_current_window() {
    crate::tests::run(|| {
        let window = PpsWindow::new();
        window.open(&fixture("utf16le-annot.pdf"), None, None);
        let first = window.imp().current_tab().unwrap();
        window.open(&fixture("ink_annotations.pdf"), None, None);
        let second = window.imp().current_tab().unwrap();
        window.present();
        wait_until("both documents", || {
            first.document().is_some() && second.document().is_some()
        });
        let tabs = window.imp().tabs.get();
        assert_eq!(tabs.n_pages(), 2);
        window.open(&fixture("utf16le-annot.pdf"), None, None);
        assert_eq!(tabs.n_pages(), 2);
        assert_eq!(window.imp().current_tab(), Some(first.clone()));
        assert_eq!(window.title().as_deref(), Some("utf16le-annot.pdf"));

        let page = tabs.page(&first);
        tabs.reorder_page(&page, 1);
        assert_eq!(tabs.nth_page(1), page);
        WidgetExt::activate_action(&window, "win.presentation", None).unwrap();
        assert!(first.is_presenting());
        let destination = PpsWindow::new();
        destination.present();
        tabs.transfer_page(&page, &destination.imp().tabs, 0);
        assert_eq!(tabs.n_pages(), 1);
        assert_eq!(destination.imp().tabs.nth_page(0), page);
        assert_eq!(destination.imp().current_tab(), Some(first));
        assert_eq!(window.imp().current_tab(), Some(second.clone()));
        assert!(!destination.imp().current_tab().unwrap().is_presenting());
        assert!(window.lookup_action("fullscreen").unwrap().is_enabled());

        WidgetExt::activate_action(&destination, "win.close", None).unwrap();
        assert!(!destination.is_visible());
        assert!(window.is_visible());
        assert_eq!(tabs.n_pages(), 1);

        let document = second.document().unwrap();
        let annotation = papers_document::AnnotationText::new(&document.page(0).unwrap());
        document
            .dynamic_cast_ref::<papers_document::DocumentAnnotations>()
            .unwrap()
            .add_annotation(&annotation);
        assert!(tabs.page(&second).indicator_icon().is_some());
        let empty = window.new_tab();
        tabs.close_page(&tabs.page(&second));
        assert_eq!(window.imp().current_tab(), Some(second.clone()));
        window.visible_dialog().unwrap().force_close();
        tabs.set_selected_page(&tabs.page(&empty));
        window.close();
        window.visible_dialog().unwrap().force_close();
        wait_until("canceled window close", || !window.imp().closing.get());
        assert!(window.is_visible());
        assert_eq!(tabs.n_pages(), 2);
        assert_eq!(window.imp().current_tab(), Some(second));

        window.close();
        let dialog = window
            .visible_dialog()
            .and_downcast::<adw::AlertDialog>()
            .unwrap();
        dialog.emit_by_name::<()>("response", &[&"no"]);
        wait_until("window close", || !window.is_visible());
    });
}
