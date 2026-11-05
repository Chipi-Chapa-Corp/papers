use crate::deps::*;

pub fn factory(view: &adw::TabView) -> gtk::SignalListItemFactory {
    let factory = gtk::SignalListItemFactory::new();
    factory.connect_bind(glib::clone!(
        #[weak]
        view,
        move |_, item| {
            let item = item.downcast_ref::<gtk::ListItem>().unwrap();
            let page = item.item().and_downcast::<adw::TabPage>().unwrap();
            let row = gtk::Box::new(gtk::Orientation::Horizontal, 6);
            row.set_margin_start(6);
            row.set_margin_end(6);

            let spinner = gtk::Spinner::new();
            for property in ["spinning", "visible"] {
                page.bind_property("loading", &spinner, property)
                    .sync_create()
                    .build();
            }
            row.append(&spinner);

            let label = gtk::Label::builder()
                .xalign(0.0)
                .hexpand(true)
                .ellipsize(gtk::pango::EllipsizeMode::End)
                .build();
            for property in ["label", "tooltip-text"] {
                page.bind_property("title", &label, property)
                    .sync_create()
                    .build();
            }
            row.append(&label);

            let indicator = gtk::Image::new();
            indicator.set_tooltip_text(Some(&gettext("Unsaved Changes")));
            page.bind_property("indicator-icon", &indicator, "gicon")
                .sync_create()
                .build();
            page.bind_property("indicator-icon", &indicator, "visible")
                .transform_to(|_, icon: Option<gio::Icon>| Some(icon.is_some()))
                .sync_create()
                .build();
            row.append(&indicator);

            let close = gtk::Button::builder()
                .icon_name("window-close-symbolic")
                .tooltip_text(gettext("Close Document"))
                .css_classes(["flat"])
                .build();
            close.connect_clicked(glib::clone!(
                #[weak]
                view,
                #[weak]
                page,
                move |_| view.close_page(&page)
            ));
            row.append(&close);
            item.set_child(Some(&row));
        }
    ));
    factory.connect_unbind(|_, item| {
        item.downcast_ref::<gtk::ListItem>()
            .unwrap()
            .set_child(gtk::Widget::NONE);
    });
    factory
}
