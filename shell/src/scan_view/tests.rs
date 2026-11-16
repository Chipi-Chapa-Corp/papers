use super::*;
use crate::tests::{run, wait_until};

fn page(view: &PpsScanView, path: &std::path::Path) {
    let uri = glib::uri_escape_string(path.to_str().unwrap(), None::<&str>, true);
    view.worker_event(&["PAGE", &uri, "150"], false);
    wait_until("working PDF", || {
        !view.imp().session.borrow().writing && view.imp().session.borrow().queue.is_empty()
    });
}

fn thumbnail_row(view: &PpsScanView, label: &str) -> gtk::Box {
    crate::tests::find_widget_where::<gtk::Box>(&view.viewer().scan_thumbnails(), &|row| {
        row.first_child()
            .is_some_and(|child| child.is::<gtk::Picture>())
            && row
                .last_child()
                .and_downcast::<gtk::Label>()
                .is_some_and(|child| child.label() == label)
    })
    .unwrap()
}

fn controller<T: IsA<gtk::EventController> + IsA<glib::Object>>(widget: &gtk::Box) -> T {
    widget
        .observe_controllers()
        .iter::<glib::Object>()
        .flatten()
        .find_map(|obj| obj.downcast::<T>().ok())
        .unwrap()
}

fn drag_thumbnail(view: &PpsScanView, from: &str, onto: &str, after: bool) {
    wait_until("thumbnail rows allocated", || {
        thumbnail_row(view, from).height() > 0 && thumbnail_row(view, onto).height() > 0
    });
    let source = controller::<gtk::DragSource>(&thumbnail_row(view, from));
    let provider = source
        .emit_by_name::<Option<gdk::ContentProvider>>("prepare", &[&0.0f64, &0.0f64])
        .unwrap();
    let value = provider.value(PpsThumbnailItem::static_type()).unwrap();
    let row = thumbnail_row(view, onto);
    let target = controller::<gtk::DropTarget>(&row);
    assert!(target.emit_by_name::<bool>(
        "drop",
        &[
            &glib::BoxedValue(value),
            &0.0f64,
            &if after {
                f64::from(row.height())
            } else {
                0.0f64
            },
        ]
    ));
    wait_until("thumbnail reorder saved to PDF", || {
        !view.imp().session.borrow().writing
    });
}

#[test]
fn scanning_uses_the_pdf_view_and_preserves_annotations_and_page_scope() {
    run(|| {
        let view: PpsScanView = glib::Object::new();
        assert_eq!(view.ui().kind.selected(), 0);
        assert_eq!(RESOLUTIONS[view.ui().image_dpi.selected() as usize], 300);
        view.enter_canvas();
        let window = adw::Window::builder()
            .default_width(1100)
            .default_height(900)
            .content(&view)
            .build();
        window.present();
        let dir = tempfile::tempdir().unwrap();
        let first = dir.path().join("landscape.png");
        let second = dir.path().join("portrait.png");
        image::RgbImage::from_pixel(300, 200, image::Rgb([255, 200, 100]))
            .save(&first)
            .unwrap();
        image::RgbImage::from_pixel(100, 300, image::Rgb([100, 200, 255]))
            .save(&second)
            .unwrap();
        page(&view, &first);
        page(&view, &second);
        assert_eq!(view.viewer().model().document().unwrap().n_pages(), 2);
        view.select(1);
        assert_eq!(view.viewer().model().page(), 0);
        view.select(2);
        assert_eq!(view.viewer().model().page(), 1);
        assert!(
            view.ui()
                .actions
                .lookup_action("save")
                .unwrap()
                .is_enabled()
        );
        crate::tests::wait_until("page-only toolbar beside sidebar", || {
            view.ui()
                .footer
                .compute_bounds(&view)
                .is_some_and(|bounds| bounds.width() > 0.0 && bounds.x() > 100.0)
        });
        assert_eq!(view.ui().footer.observe_children().n_items(), 2);
        let footer = view.ui().footer.compute_bounds(&view).unwrap();
        let page_host = view.viewer().tab_bar_host().compute_bounds(&view).unwrap();
        assert!(footer.x() >= page_host.x());
        assert!(footer.width() <= page_host.width());
        assert!(footer.x() > 100.0, "sidebar must extend beside the footer");
        let thumbnails =
            crate::tests::find_widget_where::<PpsSidebarThumbnails>(&view, &|_| true).unwrap();
        assert!(
            thumbnails.is_mapped(),
            "scans should open the thumbnails sidebar"
        );
        for button in [
            &view.ui().rotate_left,
            &view.ui().rotate_right,
            &view.ui().crop_button,
        ] {
            assert!(button.is_sensitive());
            let bounds = button.compute_bounds(&view).unwrap();
            assert!(
                bounds.y() >= footer.y()
                    && bounds.y() + bounds.height() <= footer.y() + footer.height() + 1.0
            );
        }
        let zoom = crate::tests::find_widget_where::<gtk::Button>(&view, &|b| {
            b.tooltip_text().as_deref() == Some("Zoom In")
        })
        .unwrap();
        let zoom_bounds = zoom.compute_bounds(&view).unwrap();
        assert!(zoom_bounds.y() >= footer.y());
        view.show_preferences();
        wait_until("preferences shown", || {
            view.ui().preferences_layer.is_visible()
        });
        view.ui().preferences_backdrop.emit_clicked();
        assert!(!view.ui().preferences_layer.is_visible());
        let toolbar_position = view.ui().footer.compute_bounds(&view).unwrap();
        let other = adw::Window::new();
        other.present();
        assert!(
            view.ui()
                .actions
                .lookup_action("save")
                .unwrap()
                .is_enabled()
        );
        other.destroy();
        // Cursor icons must also work in runtimes without legacy SVG pixbuf loaders.
        view.viewer()
            .activate_action("doc.toggle-edit-mode", None)
            .unwrap();
        for tool in [
            papers_view::AnnotationTool::Pencil,
            papers_view::AnnotationTool::Highlight,
            papers_view::AnnotationTool::Eraser,
        ] {
            view.viewer()
                .model()
                .annotation_model()
                .unwrap()
                .set_tool(tool);
            while glib::MainContext::default().pending() {
                glib::MainContext::default().iteration(false);
            }
        }
        view.viewer()
            .activate_action("doc.toggle-edit-mode", None)
            .unwrap();
        let doc = view.viewer().model().document().unwrap();
        let annots = doc
            .clone()
            .dynamic_cast::<papers_document::DocumentAnnotations>()
            .unwrap();
        let annotation = papers_document::AnnotationText::new(&doc.page(0).unwrap());
        annotation.set_contents("Retain this note across scan edits");
        let raw = papers_document::ffi::PpsRectangle {
            x1: 10.0,
            y1: 10.0,
            x2: 30.0,
            y2: 30.0,
        };
        let area: papers_document::Rectangle =
            unsafe { glib::translate::from_glib_none(&raw as *const _) };
        annotation.set_area(&area);
        annots.add_annotation(&annotation);
        let saved = glib::MainContext::default()
            .block_on(view.make_pdf())
            .unwrap();
        let saved_again = glib::MainContext::default()
            .block_on(view.make_pdf())
            .unwrap();
        for path in [saved, saved_again] {
            let pdf = lopdf::Document::load(path).unwrap();
            assert_eq!(
                pdf.get_page_annotations(pdf.get_pages()[&1]).unwrap().len(),
                1
            );
        }
        view.select(1);
        view.rotate(90);
        wait_until("rotated PDF", || !view.imp().session.borrow().writing);
        let pdf = view.imp().session.borrow().pdf.clone().unwrap();
        let after_rotation = view.ui().footer.compute_bounds(&view).unwrap();
        assert!((after_rotation.y() - toolbar_position.y()).abs() < 1.0);
        assert_eq!(scan_pdf::geometry(&pdf, 0).unwrap().1, 90);
        assert_eq!(scan_pdf::geometry(&pdf, 1).unwrap().1, 0);
        let saved = lopdf::Document::load(&pdf).unwrap();
        let page = saved.get_pages()[&1];
        let annotations = saved.get_page_annotations(page).unwrap();
        assert_eq!(annotations.len(), 1);
        view.ui()
            .actions
            .activate_action("scope", Some(&"document".to_variant()));
        view.rotate(-90);
        wait_until("document rotation", || !view.imp().session.borrow().writing);
        let pdf = view.imp().session.borrow().pdf.clone().unwrap();
        assert_eq!(scan_pdf::geometry(&pdf, 0).unwrap().1, 0);
        assert_eq!(scan_pdf::geometry(&pdf, 1).unwrap().1, 270);
        drag_thumbnail(&view, "2", "1", false);
        wait_until("reordered PDF", || !view.imp().session.borrow().writing);
        assert_eq!(view.imp().session.borrow().pdf_ids, [2, 1]);
        drag_thumbnail(&view, "1", "2", true);
        assert_eq!(view.imp().session.borrow().pdf_ids, [1, 2]);
        let exported = glib::MainContext::default()
            .block_on(view.make_pdf())
            .unwrap();
        assert_eq!(scan_pdf::geometry(&exported, 0).unwrap().1, 0);
        assert_eq!(scan_pdf::geometry(&exported, 1).unwrap().1, 270);
        let row = thumbnail_row(&view, "1");
        let target = controller::<gtk::DropTarget>(&row);
        assert!(
            !target.emit_by_name::<bool>(
                "drop",
                &[
                    &glib::BoxedValue(PpsThumbnailItem::new().to_value()),
                    &0.0f64,
                    &0.0f64,
                ]
            ),
            "foreign or stale thumbnail drops must be rejected"
        );
        view.ui().actions.activate_action("delete-page", None);
        wait_until("deleted PDF page", || !view.imp().session.borrow().writing);
        assert_eq!(view.viewer().model().document().unwrap().n_pages(), 1);
        view.remove_page(1);
        wait_until("empty scan", || !view.imp().session.borrow().writing);
        assert!(view.viewer().model().document().is_none());
        assert_eq!(view.ui().footer.observe_children().n_items(), 2);
        window.destroy();
    });
}

#[test]
fn progress_placeholder_and_preferences_do_not_change_document_layout() {
    run(|| {
        let view: PpsScanView = glib::Object::new();
        view.enter_canvas();
        let window = adw::Window::builder()
            .default_width(1028)
            .default_height(1438)
            .content(&view)
            .build();
        window.present();
        wait_until("empty scan layout", || view.ui().footer.width() > 0);
        let selector =
            crate::tests::find_widget_where::<PpsPageSelector>(&view, &|_| true).unwrap();
        assert!(!selector.is_sensitive());
        assert!(!selector.is_mapped());
        let sidebar = crate::tests::find_widget_where::<PpsSidebar>(&view, &|_| true).unwrap();
        assert!(!sidebar.is_mapped());
        view.imp().session.borrow_mut().scanning = true;
        view.worker_event(&["START", "1"], false);
        assert_eq!(view.ui().footer.observe_children().n_items(), 2);
        view.worker_event(&["PROGRESS", "0.6"], false);
        assert!((view.ui().progress.fraction() - 0.6).abs() < 0.001);
        let progress = view.ui().progress.compute_bounds(&view).unwrap();
        let button = view.ui().scan.compute_bounds(&view).unwrap();
        assert!(
            progress.y() + progress.height() <= button.y(),
            "progress must be separate from button"
        );
        view.ui().kind.set_selected(1);
        assert!(view.ui().text_dpi.property::<bool>("visible"));
        assert!(!view.ui().image_dpi.property::<bool>("visible"));
        assert!(view.ui().settings_scroll.child().is_some());
        view.imp().session.borrow_mut().scanning = false;
        window.destroy();
    });
}
