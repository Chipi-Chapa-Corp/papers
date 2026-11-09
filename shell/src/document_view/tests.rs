use super::*;

#[test]
fn closing_reports_the_result_without_destroying_the_container() {
    crate::tests::run(|| {
        let view = PpsDocumentView::new();
        let window = adw::Window::builder()
            .title("Container")
            .content(&view)
            .build();
        window.present();

        let results = std::rc::Rc::new(RefCell::new(Vec::<bool>::new()));
        view.connect_local(
            "close-finished",
            false,
            glib::clone!(
                #[strong]
                results,
                move |args| {
                    results.borrow_mut().push(args[1].get().unwrap());
                    None
                }
            ),
        );

        view.request_close();
        assert_eq!(*results.borrow(), [true]);
        assert!(window.is_visible());

        let file = crate::tests::fixture("utf16le-annot.pdf");
        let job = papers_view::JobLoad::new();
        job.set_uri(&file.uri());
        job.run();
        job.is_succeeded().unwrap();
        let document = job.loaded_document().unwrap();
        view.set_filenames("utf16le-annot.pdf", "utf16le-annot.pdf");
        view.open_document(&document, None, None, WindowRunMode::Normal);
        assert_eq!(view.title(), "utf16le-annot.pdf");
        assert_eq!(window.title().as_deref(), Some("Container"));
        assert!(!view.modified());

        let copy = PpsDocumentView::new();
        let copy_window = adw::Window::builder().content(&copy).build();
        copy.open_copy_from(&view, None);
        assert_eq!(copy.model().document(), Some(document.clone()));
        copy.model().set_scale(1.75);
        assert_ne!(copy.model().scale(), view.model().scale());
        copy_window.destroy();

        let annotation = AnnotationText::new(&document.page(0).unwrap());
        document
            .dynamic_cast_ref::<DocumentAnnotations>()
            .unwrap()
            .add_annotation(&annotation);
        assert!(view.modified());

        let dirty_copy = PpsDocumentView::new();
        let dirty_window = adw::Window::builder().content(&dirty_copy).build();
        dirty_copy.open_copy_from(&view, None);
        assert!(dirty_copy.modified());
        let (saved, stream) = gio::File::new_tmp(Some("papers-dirty-copy-XXXXXX.pdf")).unwrap();
        stream.close(gio::Cancellable::NONE).unwrap();
        document.save(&saved.uri()).unwrap();
        assert!(!view.modified());
        assert!(!dirty_copy.modified());
        saved.delete(gio::Cancellable::NONE).unwrap();
        dirty_window.destroy();
        document
            .dynamic_cast_ref::<DocumentAnnotations>()
            .unwrap()
            .add_annotation(&AnnotationText::new(&document.page(0).unwrap()));
        assert!(view.modified());

        view.request_close();
        let dialog = window.visible_dialog().unwrap();
        view.request_close();
        assert_eq!(window.visible_dialog(), Some(dialog.clone()));
        dialog.force_close();
        assert_eq!(*results.borrow(), [true, false]);
        assert!(window.is_visible());

        view.request_close();
        let dialog = window
            .visible_dialog()
            .and_downcast::<adw::AlertDialog>()
            .unwrap();
        dialog.emit_by_name::<()>("response", &[&"no"]);
        dialog.force_close();
        assert_eq!(*results.borrow(), [true, false, true]);
        assert!(window.is_visible());
        window.destroy();
    });
}

#[test]
fn sidebar_resizing_preserves_adaptive_layout() {
    crate::tests::run(|| {
        let view = PpsDocumentView::new();
        let window = adw::Window::builder()
            .default_width(1200)
            .default_height(800)
            .content(&view)
            .build();
        let split_view = view.imp().split_view.get();
        split_view.set_show_sidebar(true);
        window.present();
        crate::tests::wait_until("expanded sidebar", || {
            !split_view.is_collapsed() && split_view.sidebar().unwrap().width() >= 210
        });

        let handle = view.imp().sidebar_resize_handle.get();
        let drag = split_view
            .observe_controllers()
            .iter::<glib::Object>()
            .find_map(|controller| controller.unwrap().downcast::<gtk::GestureDrag>().ok())
            .unwrap();
        let resize = |offset: f64| {
            let point = handle
                .compute_point(&split_view, &gtk::graphene::Point::new(3.0, 100.0))
                .unwrap();
            drag.emit_by_name::<()>("drag-begin", &[&(point.x() as f64), &(point.y() as f64)]);
            drag.emit_by_name::<()>("drag-update", &[&offset, &0.0_f64]);
        };
        let sidebar_width = || split_view.sidebar().unwrap().width();
        let initial_width = sidebar_width();
        resize(120.0);
        crate::tests::wait_until("wider sidebar", || sidebar_width() == initial_width + 120);

        resize(-1000.0);
        crate::tests::wait_until("minimum sidebar width", || sidebar_width() == 210);
        resize(1000.0);
        crate::tests::wait_until("maximum sidebar width", || sidebar_width() == 600);

        view.imp()
            .sidebar_stack
            .set_visible_child(&view.imp().find_sidebar.get());
        resize(-150.0);
        crate::tests::wait_until("resized search sidebar", || sidebar_width() == 450);
        split_view.set_show_sidebar(false);
        split_view.set_show_sidebar(true);
        crate::tests::wait_until("restored sidebar width", || sidebar_width() == 450);

        window.set_default_size(600, 800);
        crate::tests::wait_until("collapsed sidebar", || split_view.is_collapsed());
        assert!(!handle.is_visible());
        split_view.set_show_sidebar(true);
        crate::tests::wait_until("overlay sidebar width", || sidebar_width() == 300);
        resize(100.0);
        assert_eq!(sidebar_width(), 300);

        window.set_default_size(500, 800);
        crate::tests::wait_until("narrow layout", || !view.imp().page_selector.is_visible());
        assert!(!handle.is_visible());
        assert_eq!(split_view.max_sidebar_width(), 300.0);

        window.set_default_size(1200, 800);
        crate::tests::wait_until("expanded sidebar width restored", || {
            !split_view.is_collapsed() && sidebar_width() == 450
        });
        assert!(handle.is_visible());

        split_view.set_direction(TextDirection::Rtl);
        resize(-100.0);
        crate::tests::wait_until("right-to-left sidebar resize", || sidebar_width() == 550);
        window.destroy();
    });
}
