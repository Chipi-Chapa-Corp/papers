// SPDX-License-Identifier: GPL-2.0-or-later
use crate::deps::*;
use crate::scan_document::{self, ScanPage};
use crate::scan_pdf::{self, Change};
use std::cell::{Cell, OnceCell};
use std::collections::VecDeque;
use std::path::PathBuf;
use std::time::Duration;

#[cfg(test)]
mod tests;

#[derive(Default, Debug)]
struct Session {
    canvas: bool,
    devices: Vec<(String, String)>,
    pages: Vec<ScanPage>,
    selected: Option<u64>,
    next_id: u64,
    revision: u64,
    dirty: bool,
    scanning: bool,
    stopping: bool,
    exporting: bool,
    writing: bool,
    queue: VecDeque<Change>,
    pdf: Option<PathBuf>,
    pdf_ids: Vec<u64>,
    process: Option<gio::Subprocess>,
    discovery: Option<gio::Subprocess>,
    directory: Option<PathBuf>,
}

#[derive(Debug)]
struct Controls {
    stack: gtk::Stack,
    toolbar: adw::ToolbarView,
    setup_form: gtk::Box,
    viewer: PpsDocumentView,
    toast: adw::ToastOverlay,
    scanner: adw::ComboRow,
    devices: gtk::StringList,
    spinner: adw::Spinner,
    refresh: gtk::Button,
    preferences: gtk::Box,
    settings_scroll: gtk::ScrolledWindow,
    kind: adw::ComboRow,
    mode: adw::ComboRow,
    sides: adw::ComboRow,
    paper: adw::ComboRow,
    delay: adw::SpinRow,
    text_dpi: adw::ComboRow,
    image_dpi: adw::ComboRow,
    brightness: gtk::Scale,
    contrast: gtk::Scale,
    compression: gtk::Scale,
    setup_scan: gtk::Button,
    scan: gtk::Button,
    scan_label: gtk::Label,
    scope: adw::ToggleGroup,
    rotate_left: gtk::Button,
    rotate_right: gtk::Button,
    crop_button: gtk::Button,
    footer: gtk::Box,
    preferences_layer: gtk::Overlay,
    preferences_backdrop: gtk::Button,
    status: gtk::Label,
    progress: gtk::ProgressBar,
    actions: gio::SimpleActionGroup,
}

mod imp {
    use super::*;
    #[derive(Default, Debug)]
    pub struct PpsScanView {
        pub(super) controls: OnceCell<Controls>,
        pub(super) session: RefCell<Session>,
        pub(super) closing: Cell<bool>,
    }
    #[glib::object_subclass]
    impl ObjectSubclass for PpsScanView {
        const NAME: &'static str = "PpsScanView";
        type Type = super::PpsScanView;
        type ParentType = adw::Bin;
    }
    impl ObjectImpl for PpsScanView {
        fn signals() -> &'static [Signal] {
            static SIGNALS: OnceLock<Vec<Signal>> = OnceLock::new();
            SIGNALS.get_or_init(|| {
                vec![
                    Signal::builder("changed").build(),
                    Signal::builder("start-scan").build(),
                    Signal::builder("back").build(),
                    Signal::builder("close-finished")
                        .param_types([bool::static_type()])
                        .build(),
                ]
            })
        }
        fn constructed(&self) {
            self.parent_constructed();
            self.obj().build();
        }
        fn dispose(&self) {
            let mut state = self.session.borrow_mut();
            for process in [&state.process, &state.discovery].into_iter().flatten() {
                process.force_exit();
            }
            if let Some(directory) = state.directory.take() {
                let _ = std::fs::remove_dir_all(directory);
            }
        }
    }
    impl WidgetImpl for PpsScanView {}
    impl BinImpl for PpsScanView {}
}
glib::wrapper! {
    pub struct PpsScanView(ObjectSubclass<imp::PpsScanView>)
        @extends gtk::Widget, adw::Bin,
        @implements gtk::Accessible, gtk::Buildable, gtk::ConstraintTarget;
}

const RESOLUTIONS: [u32; 7] = [75, 150, 200, 300, 600, 1200, 2400];
const PAPER_CODES: [&str; 8] = ["auto", "a6", "a5", "a4", "a3", "letter", "legal", "4x6"];
fn combo(title: &str, items: &[&str]) -> adw::ComboRow {
    adw::ComboRow::builder()
        .title(title)
        .model(&gtk::StringList::new(items))
        .build()
}
fn icon_button(icon: &str, tooltip: &str) -> gtk::Button {
    gtk::Button::builder()
        .icon_name(icon)
        .tooltip_text(tooltip)
        .valign(gtk::Align::Center)
        .css_classes(["circular"])
        .build()
}
fn resolution(default: u32) -> adw::ComboRow {
    let labels: Vec<_> = RESOLUTIONS
        .iter()
        .map(|dpi| {
            let pattern = if *dpi == default {
                gettext("{dpi} dpi (default)")
            } else {
                gettext("{dpi} dpi")
            };
            pattern.replace("{dpi}", &dpi.to_string())
        })
        .collect();
    let row = combo(
        &gettext("Resolution"),
        &labels.iter().map(String::as_str).collect::<Vec<_>>(),
    );
    row.set_selected(RESOLUTIONS.iter().position(|d| *d == default).unwrap() as u32);
    row
}
fn quality_scale(
    min: f64,
    max: f64,
    initial: f64,
    low: &str,
    high: &str,
    marks: &[f64],
    draw: bool,
) -> gtk::Scale {
    let scale = gtk::Scale::with_range(gtk::Orientation::Horizontal, min, max, 1.0);
    scale.set_value(initial);
    scale.set_digits(0);
    scale.set_draw_value(draw);
    scale.set_hexpand(true);
    scale.set_width_request(180);
    scale.set_valign(gtk::Align::Center);
    scale.add_mark(
        min,
        gtk::PositionType::Bottom,
        Some(&format!("<small>{low}</small>")),
    );
    for mark in marks {
        scale.add_mark(*mark, gtk::PositionType::Bottom, None);
    }
    scale.add_mark(
        max,
        gtk::PositionType::Bottom,
        Some(&format!("<small>{high}</small>")),
    );
    scale
}
fn quality_row(title: &str, scale: &gtk::Scale) -> adw::ActionRow {
    let row = adw::ActionRow::builder().title(title).build();
    row.add_suffix(scale);
    row
}

impl PpsScanView {
    fn ui(&self) -> &Controls {
        self.imp().controls.get().unwrap()
    }
    fn build(&self) {
        let toolbar = adw::ToolbarView::new();
        let header = adw::HeaderBar::new();
        header.set_title_widget(Some(&adw::WindowTitle::new(
            &gettext("Scan a Document"),
            "",
        )));
        let back = icon_button("go-previous-symbolic", &gettext("Back"));
        header.pack_start(&back);
        let new_tab = icon_button("list-add-symbolic", &gettext("New Tab"));
        new_tab.set_action_name(Some("win.new-tab"));
        header.pack_start(&new_tab);
        toolbar.add_top_bar(&header);
        let setup_form = gtk::Box::builder()
            .orientation(gtk::Orientation::Vertical)
            .spacing(20)
            .margin_top(24)
            .margin_bottom(24)
            .margin_start(20)
            .margin_end(20)
            .build();
        let clamp = adw::Clamp::builder()
            .maximum_size(680)
            .valign(gtk::Align::Center)
            .vexpand(true)
            .child(&setup_form)
            .build();
        let setup_scroll = gtk::ScrolledWindow::builder()
            .hscrollbar_policy(gtk::PolicyType::Never)
            .child(&clamp)
            .build();
        toolbar.set_content(Some(&setup_scroll));
        let preferences = gtk::Box::new(gtk::Orientation::Vertical, 16);
        setup_form.append(&preferences);
        let devices = gtk::StringList::new(&[]);
        let scanner = adw::ComboRow::builder()
            .title(gettext("Scanner"))
            .model(&devices)
            .build();
        let spinner = adw::Spinner::builder()
            .width_request(20)
            .height_request(20)
            .valign(gtk::Align::Center)
            .build();
        let refresh = icon_button("view-refresh-symbolic", &gettext("Refresh Scanners"));
        scanner.add_suffix(&spinner);
        scanner.add_suffix(&refresh);
        let group = adw::PreferencesGroup::new();
        group.add(&scanner);
        preferences.append(&group);
        let kind = combo(
            &gettext("Scan Type"),
            &[&gettext("Image"), &gettext("Text")],
        );
        let mode = combo(
            &gettext("Scan"),
            &[
                &gettext("Single Page"),
                &gettext("All from Feeder"),
                &gettext("Multiple from Flatbed"),
            ],
        );
        let advanced = adw::ExpanderRow::builder()
            .title(gettext("Advanced"))
            .build();
        let sides = combo(
            &gettext("Scan Sides"),
            &[&gettext("Front"), &gettext("Back"), &gettext("Both Sides")],
        );
        sides.set_sensitive(false);
        let paper = combo(
            &gettext("Page Size"),
            &[
                &gettext("Automatic"),
                "A6",
                "A5",
                "A4",
                "A3",
                &gettext("Letter"),
                &gettext("Legal"),
                "4×6",
            ],
        );
        let delay = adw::SpinRow::with_range(1.0, 120.0, 1.0);
        delay.set_title(&gettext("Delay Between Pages"));
        delay.set_subtitle(&gettext("Seconds"));
        delay.set_value(3.0);
        delay.set_visible(false);
        let text_dpi = resolution(150);
        text_dpi.set_visible(false);
        let image_dpi = resolution(300);
        let brightness = quality_scale(
            -100.0,
            100.0,
            0.0,
            &gettext("Darker"),
            &gettext("Lighter"),
            &[0.0],
            true,
        );
        let contrast = quality_scale(
            -100.0,
            100.0,
            0.0,
            &gettext("Less"),
            &gettext("More"),
            &[0.0],
            true,
        );
        let compression = quality_scale(
            0.0,
            100.0,
            75.0,
            &gettext("Minimum size"),
            &gettext("Full detail"),
            &[75.0, 90.0],
            false,
        );
        for row in [
            sides.upcast_ref::<gtk::Widget>(),
            paper.upcast_ref(),
            delay.upcast_ref(),
            text_dpi.upcast_ref(),
            image_dpi.upcast_ref(),
        ] {
            advanced.add_row(row);
        }
        advanced.add_row(&quality_row(&gettext("Brightness"), &brightness));
        advanced.add_row(&quality_row(&gettext("Contrast"), &contrast));
        advanced.add_row(&quality_row(&gettext("Compression"), &compression));
        let group = adw::PreferencesGroup::new();
        group.add(&kind);
        group.add(&mode);
        group.add(&advanced);
        preferences.append(&group);
        let status = gtk::Label::builder()
            .wrap(true)
            .selectable(true)
            .visible(false)
            .build();
        status.add_css_class("error");
        setup_form.append(&status);
        let setup_scan = gtk::Button::builder()
            .label(gettext("Scan"))
            .halign(gtk::Align::Center)
            .css_classes(["suggested-action", "pill"])
            .width_request(130)
            .build();
        setup_form.append(&setup_scan);

        let viewer = PpsDocumentView::new();
        let footer = gtk::Box::builder()
            .orientation(gtk::Orientation::Vertical)
            .spacing(6)
            .margin_top(8)
            .margin_bottom(8)
            .margin_start(16)
            .margin_end(16)
            .build();
        let progress_slot = gtk::Box::builder().height_request(4).build();
        let progress = gtk::ProgressBar::builder()
            .hexpand(true)
            .can_target(false)
            .visible(false)
            .build();
        progress_slot.append(&progress);
        footer.append(&progress_slot);
        let toolbar_row = gtk::Box::new(gtk::Orientation::Horizontal, 12);
        let tools = adw::WrapBox::builder()
            .hexpand(true)
            .child_spacing(12)
            .line_spacing(8)
            .natural_line_length(700)
            .justify(adw::JustifyMode::Spread)
            .justify_last_line(true)
            .build();
        let page_tools = gtk::Box::new(gtk::Orientation::Horizontal, 6);
        let rotate_left = icon_button("object-rotate-left-symbolic", &gettext("Rotate Left"));
        let rotate_right = icon_button("object-rotate-right-symbolic", &gettext("Rotate Right"));
        let crop_button = icon_button("crop-symbolic", &gettext("Crop Page"));
        for (button, action) in [
            (&rotate_left, "scan.rotate-left"),
            (&rotate_right, "scan.rotate-right"),
            (&crop_button, "scan.crop"),
        ] {
            button.set_size_request(36, 36);
            button.set_action_name(Some(action));
            page_tools.append(button);
        }
        let scope = adw::ToggleGroup::new();
        scope.add(
            adw::Toggle::builder()
                .label(gettext("Page"))
                .name("page")
                .build(),
        );
        scope.add(
            adw::Toggle::builder()
                .label(gettext("Document"))
                .name("document")
                .build(),
        );
        scope.set_active_name(Some("page"));
        scope.set_valign(gtk::Align::Center);
        scope.set_tooltip_text(Some(&gettext(
            "Apply rotation and cropping to this page or the entire document",
        )));
        page_tools.append(&scope);
        tools.append(&page_tools);
        viewer.configure_for_scanning();
        tools.append(&viewer.dock_scan_view_controls());
        let acquisition = gtk::Box::new(gtk::Orientation::Horizontal, 8);
        let preferences_button =
            icon_button("preferences-system-symbolic", &gettext("Scan Preferences"));
        preferences_button.set_size_request(36, 36);
        preferences_button.set_action_name(Some("scan.preferences"));
        acquisition.append(&preferences_button);
        let scan_label = gtk::Label::new(Some(&gettext("Scan next")));
        let scan = gtk::Button::builder()
            .child(&scan_label)
            .width_request(160)
            .height_request(44)
            .valign(gtk::Align::Center)
            .css_classes(["suggested-action", "pill"])
            .build();
        acquisition.append(&scan);
        acquisition.set_valign(gtk::Align::End);
        toolbar_row.append(&tools);
        toolbar_row.append(&acquisition);
        footer.append(&toolbar_row);
        viewer.tab_bar_host().add_bottom_bar(&footer);
        let settings_scroll = gtk::ScrolledWindow::builder()
            .min_content_width(360)
            .width_request(440)
            .max_content_height(520)
            .propagate_natural_height(true)
            .hscrollbar_policy(gtk::PolicyType::Never)
            .build();
        let preferences_layer = gtk::Overlay::builder().visible(false).build();
        let preferences_backdrop = gtk::Button::builder()
            .focusable(false)
            .css_classes(["scan-preferences-backdrop"])
            .build();
        preferences_layer.set_child(Some(&preferences_backdrop));
        let card = gtk::Box::builder()
            .orientation(gtk::Orientation::Vertical)
            .spacing(12)
            .margin_start(24)
            .margin_end(24)
            .margin_top(24)
            .margin_bottom(24)
            .halign(gtk::Align::Center)
            .valign(gtk::Align::Center)
            .css_classes(["card", "scan-preferences-card"])
            .build();
        let card_header = gtk::Box::new(gtk::Orientation::Horizontal, 12);
        let title = gtk::Label::builder()
            .label(gettext("Scan Preferences"))
            .hexpand(true)
            .xalign(0.0)
            .css_classes(["heading"])
            .build();
        card_header.append(&title);
        let close_preferences = icon_button("window-close-symbolic", &gettext("Close Preferences"));
        card_header.append(&close_preferences);
        card.append(&card_header);
        card.append(&settings_scroll);
        preferences_layer.add_overlay(&card);
        let canvas = gtk::Overlay::new();
        canvas.set_child(Some(&viewer));
        canvas.add_overlay(&preferences_layer);
        let toast = adw::ToastOverlay::new();
        toast.set_child(Some(&canvas));
        let stack = gtk::Stack::new();
        stack.add_named(&toolbar, Some("setup"));
        stack.add_named(&toast, Some("canvas"));
        stack.set_visible_child_name("setup");
        self.set_child(Some(&stack));
        self.imp()
            .controls
            .set(Controls {
                stack,
                toolbar,
                setup_form,
                viewer,
                toast,
                scanner,
                devices,
                spinner,
                refresh,
                preferences,
                settings_scroll,
                kind,
                mode,
                sides,
                paper,
                delay,
                text_dpi,
                image_dpi,
                brightness,
                contrast,
                compression,
                setup_scan,
                scan,
                scan_label,
                scope,
                rotate_left,
                rotate_right,
                crop_button,
                footer,
                preferences_layer,
                preferences_backdrop,
                status,
                progress,
                actions: gio::SimpleActionGroup::new(),
            })
            .unwrap();
        let ui = self.ui();
        back.connect_clicked(glib::clone!(
            #[weak(rename_to=obj)]
            self,
            move |_| obj.confirm_leave(true)
        ));
        ui.refresh.connect_clicked(glib::clone!(
            #[weak(rename_to=obj)]
            self,
            move |_| obj.discover()
        ));
        ui.kind.connect_selected_notify(glib::clone!(
            #[weak(rename_to=obj)]
            self,
            move |row| {
                obj.ui().text_dpi.set_visible(row.selected() == 1);
                obj.ui().image_dpi.set_visible(row.selected() == 0);
            }
        ));
        ui.mode.connect_selected_notify(glib::clone!(
            #[weak(rename_to=obj)]
            self,
            move |row| {
                obj.ui().delay.set_visible(row.selected() == 2);
                obj.ui().sides.set_sensitive(row.selected() == 1);
            }
        ));
        ui.setup_scan.connect_clicked(glib::clone!(
            #[weak(rename_to=obj)]
            self,
            move |_| obj.emit_by_name::<()>("start-scan", &[])
        ));
        ui.scan.connect_clicked(glib::clone!(
            #[weak(rename_to=obj)]
            self,
            move |_| {
                if obj.imp().session.borrow().scanning {
                    obj.stop();
                } else {
                    obj.scan(true);
                }
            }
        ));
        for button in [&ui.preferences_backdrop, &close_preferences] {
            button.connect_clicked(glib::clone!(
                #[weak(rename_to=obj)]
                self,
                move |_| obj.close_preferences()
            ));
        }
        let escape = gtk::EventControllerKey::new();
        escape.set_propagation_phase(gtk::PropagationPhase::Capture);
        escape.connect_key_pressed(glib::clone!(
            #[weak(rename_to=obj)]
            self,
            #[upgrade_or]
            glib::Propagation::Proceed,
            move |_, key, _, _| {
                if key == gdk::Key::Escape && obj.ui().preferences_layer.is_visible() {
                    obj.close_preferences();
                    glib::Propagation::Stop
                } else {
                    glib::Propagation::Proceed
                }
            }
        ));
        self.add_controller(escape);
        for name in [
            "save",
            "email",
            "preferences",
            "rotate-left",
            "rotate-right",
            "crop",
            "delete-page",
            "move-earlier",
            "move-later",
        ] {
            let action = gio::SimpleAction::new(name, None);
            action.connect_activate(glib::clone!(
                #[weak(rename_to=obj)]
                self,
                move |_, _| match name {
                    "save" => {
                        glib::spawn_future_local(async move {
                            obj.save_pdf().await;
                        });
                    }
                    "email" => obj.share(true),
                    "preferences" => obj.show_preferences(),
                    "rotate-left" => obj.rotate(-90),
                    "rotate-right" => obj.rotate(90),
                    "crop" => obj.show_crop(),
                    "delete-page" => {
                        let selected = obj.imp().session.borrow().selected;
                        if let Some(id) = selected {
                            obj.remove_page(id);
                        }
                    }
                    _ => obj.move_selected(name == "move-later"),
                }
            ));
            ui.actions.add_action(&action);
        }
        let scope = gio::SimpleAction::new_stateful(
            "scope",
            Some(glib::VariantTy::STRING),
            &"page".to_variant(),
        );
        scope.connect_activate(glib::clone!(
            #[weak(rename_to=obj)]
            self,
            move |action, parameter| {
                if let Some(value) = parameter
                    && matches!(value.str(), Some("page" | "document"))
                {
                    action.set_state(value);
                    obj.ui().scope.set_active_name(value.str());
                }
            }
        ));
        ui.actions.add_action(&scope);
        ui.scope.connect_active_name_notify(glib::clone!(
            #[weak(rename_to=obj)]
            self,
            move |group| {
                if let Some(name) = group.active_name() {
                    obj.ui()
                        .actions
                        .activate_action("scope", Some(&name.to_variant()));
                }
            }
        ));
        self.insert_action_group("scan", Some(&ui.actions));
        ui.viewer.scan_thumbnails().connect_local(
            "page-reordered",
            false,
            glib::clone!(
                #[weak(rename_to=obj)]
                self,
                #[upgrade_or_default]
                move |values| {
                    let from = values[1].get::<u32>().unwrap() as usize;
                    let before = values[2].get::<u32>().unwrap() as usize;
                    let state = obj.imp().session.borrow();
                    if state.writing {
                        return None;
                    }
                    let id = state.pdf_ids.get(from).copied();
                    let before = state.pdf_ids.get(before).copied();
                    drop(state);
                    if let Some(id) = id {
                        obj.reorder(id, before);
                    }
                    None
                }
            ),
        );
        ui.viewer.model().connect_page_notify(glib::clone!(
            #[weak(rename_to=obj)]
            self,
            move |model| {
                let id = obj
                    .imp()
                    .session
                    .borrow()
                    .pdf_ids
                    .get(model.page().max(0) as usize)
                    .copied();
                if id.is_some() {
                    obj.imp().session.borrow_mut().selected = id;
                }
            }
        ));
        ui.viewer.connect_modified_notify(glib::clone!(
            #[weak(rename_to=obj)]
            self,
            move |view| {
                if view.modified() {
                    obj.changed();
                }
            }
        ));
        ui.viewer.connect_local(
            "close-finished",
            false,
            glib::clone!(
                #[weak(rename_to=obj)]
                self,
                #[upgrade_or_default]
                move |_| {
                    obj.confirm_leave(false);
                    None
                }
            ),
        );
        let weak = self.downgrade();
        glib::timeout_add_local(Duration::from_millis(80), move || {
            let Some(obj) = weak.upgrade() else {
                return glib::ControlFlow::Break;
            };
            if obj.imp().session.borrow().scanning && obj.ui().progress.fraction() == 0.0 {
                obj.ui().progress.pulse();
            }
            glib::ControlFlow::Continue
        });
        self.update_controls();
    }
    pub fn viewer(&self) -> PpsDocumentView {
        self.ui().viewer.clone()
    }
    pub fn toolbar(&self) -> adw::ToolbarView {
        if self.is_canvas() {
            self.ui().viewer.tab_bar_host()
        } else {
            self.ui().toolbar.clone()
        }
    }
    pub fn is_canvas(&self) -> bool {
        self.imp().session.borrow().canvas
    }
    pub fn dirty(&self) -> bool {
        self.imp().session.borrow().dirty || self.ui().viewer.modified()
    }
    pub fn busy(&self) -> bool {
        let s = self.imp().session.borrow();
        s.scanning || s.exporting || s.writing
    }
    pub fn enter_canvas(&self) {
        self.imp().session.borrow_mut().canvas = true;
        self.ui().setup_form.remove(&self.ui().preferences);
        self.ui()
            .settings_scroll
            .set_child(Some(&self.ui().preferences));
        self.ui().stack.set_visible_child_name("canvas");
        self.update_controls();
    }
    pub fn begin_scan(&self) {
        self.scan(false);
    }
    fn changed(&self) {
        let mut s = self.imp().session.borrow_mut();
        s.dirty = true;
        s.revision += 1;
        drop(s);
        self.update_controls();
        self.emit_by_name::<()>("changed", &[]);
    }
    fn show_error(&self, text: &str) {
        warn!("Scanner: {text}");
        if self.is_canvas() {
            let dialog = adw::AlertDialog::builder()
                .heading(gettext("Scanning Failed"))
                .body(text)
                .build();
            dialog.add_response("close", &gettext("Close"));
            dialog.present(Some(self));
        } else {
            self.ui().status.set_label(text);
            self.ui().status.set_visible(true);
        }
    }
    fn update_controls(&self) {
        let state = self.imp().session.borrow();
        let ui = self.ui();
        ui.footer.set_visible(state.canvas);
        let ready = !state.devices.is_empty() && !state.exporting;
        ui.setup_scan.set_sensitive(ready && !state.scanning);
        ui.scan.set_sensitive(state.scanning || ready);
        ui.scan_label.set_label(&if state.scanning {
            gettext("Stop scanning")
        } else {
            gettext("Scan next")
        });
        if state.scanning {
            ui.scan.add_css_class("destructive-action");
            ui.scan.remove_css_class("suggested-action");
        } else {
            ui.scan.remove_css_class("destructive-action");
            ui.scan.add_css_class("suggested-action");
        }
        ui.preferences.set_sensitive(!state.scanning);
        ui.progress.set_visible(state.scanning);
        let has_pdf = ui.viewer.model().document().is_some();
        let editable = state.selected.is_some() && !state.writing;
        ui.viewer.scan_thumbnails().set_reorder_enabled(editable);
        for button in [&ui.rotate_left, &ui.rotate_right, &ui.crop_button] {
            button.set_sensitive(editable);
        }
        ui.scope.set_sensitive(has_pdf);
        for name in ["save", "email"] {
            ui.actions
                .lookup_action(name)
                .unwrap()
                .downcast::<gio::SimpleAction>()
                .unwrap()
                .set_enabled(has_pdf && !state.exporting);
        }
        for name in [
            "rotate-left",
            "rotate-right",
            "crop",
            "delete-page",
            "move-earlier",
            "move-later",
        ] {
            ui.actions
                .lookup_action(name)
                .unwrap()
                .downcast::<gio::SimpleAction>()
                .unwrap()
                .set_enabled(state.selected.is_some() && !state.writing);
        }
    }
    fn worker(&self, args: &[String]) -> Result<gio::Subprocess, glib::Error> {
        let worker = std::env::var("DOKUMENT_SCAN_WORKER").unwrap_or_else(|_| SCAN_WORKER.into());
        let mut argv = vec![std::ffi::OsStr::new(&worker)];
        argv.extend(args.iter().map(std::ffi::OsStr::new));
        gio::Subprocess::newv(&argv, gio::SubprocessFlags::STDOUT_PIPE)
    }
    pub fn discover(&self) {
        if self.imp().session.borrow().discovery.is_some() {
            return;
        }

        self.ui().status.set_label("");
        self.ui()
            .scanner
            .set_subtitle(&gettext("Searching for scanners…"));
        self.ui().spinner.set_visible(true);
        self.ui().refresh.set_sensitive(false);
        self.ui()
            .devices
            .splice(0, self.ui().devices.n_items(), &[]);
        self.imp().session.borrow_mut().devices.clear();
        let process = match self.worker(&["--list".into()]) {
            Ok(process) => process,
            Err(_) => {
                self.ui().status.set_label(&gettext("Scanning support is unavailable. Install Dokument’s scanner support and try again."));
                self.ui()
                    .scanner
                    .set_subtitle(&gettext("No scanner support installed"));
                self.ui().spinner.set_visible(false);
                self.ui().refresh.set_sensitive(true);
                self.update_controls();
                return;
            }
        };
        self.imp().session.borrow_mut().discovery = Some(process.clone());
        let weak = self.downgrade();
        let timeout_process = process.clone();
        glib::timeout_add_local_once(Duration::from_secs(60), move || {
            if let Some(obj) = weak.upgrade()
                && obj.imp().session.borrow().discovery.as_ref() == Some(&timeout_process)
            {
                timeout_process.force_exit();
            }
        });
        self.read_worker(process, true);
        self.update_controls();
    }

    fn scan(&self, additional_page: bool) {
        if self.imp().session.borrow().scanning || self.imp().session.borrow().exporting {
            return;
        }
        let ui = self.ui();

        let index = ui.scanner.selected() as usize;
        let Some(device) = self
            .imp()
            .session
            .borrow()
            .devices
            .get(index)
            .map(|d| d.0.clone())
        else {
            return;
        };
        let root = match self.ensure_directory() {
            Ok(root) => root,
            Err(e) => {
                self.show_error(&e);
                return;
            }
        };
        let directory = root.join(format!("batch-{}", self.imp().session.borrow().next_id + 1));
        if let Err(e) = std::fs::create_dir_all(&directory) {
            self.show_error(&e.to_string());
            return;
        }
        let mode = if additional_page && ui.mode.selected() != 1 {
            0
        } else {
            ui.mode.selected()
        };
        let dpi = RESOLUTIONS[if ui.kind.selected() == 0 {
            ui.image_dpi.selected()
        } else {
            ui.text_dpi.selected()
        } as usize];
        let mut args = vec![
            "--device".into(),
            device,
            "--directory".into(),
            directory.to_string_lossy().into_owned(),
            "--kind".into(),
            if ui.kind.selected() == 1 {
                "text"
            } else {
                "image"
            }
            .into(),
            "--mode".into(),
            mode.to_string(),
            "--dpi".into(),
            dpi.to_string(),
            "--paper".into(),
            PAPER_CODES[ui.paper.selected() as usize].into(),
            "--sides".into(),
            ["front", "back", "both"][ui.sides.selected() as usize].into(),
            "--delay".into(),
            (ui.delay.value() as u32).to_string(),
            "--brightness".into(),
            (ui.brightness.value() as i32).to_string(),
            "--contrast".into(),
            (ui.contrast.value() as i32).to_string(),
        ];
        if additional_page {
            args.push("--single".into());
        }
        match self.worker(&args) {
            Ok(process) => {
                let mut state = self.imp().session.borrow_mut();
                state.scanning = true;
                state.stopping = false;
                state.process = Some(process.clone());
                drop(state);
                ui.progress.set_fraction(0.0);
                self.update_controls();
                self.emit_by_name::<()>("changed", &[]);
                self.read_worker(process, false);
            }
            Err(e) => self.show_error(&e.to_string()),
        }
    }

    fn stop(&self) {
        let process = self.imp().session.borrow().process.clone();
        if let Some(process) = process {
            self.imp().session.borrow_mut().stopping = true;
            process.send_signal(2);

            let weak = self.downgrade();
            glib::timeout_add_local_once(Duration::from_secs(2), move || {
                if let Some(obj) = weak.upgrade()
                    && obj.imp().session.borrow().process.as_ref() == Some(&process)
                {
                    process.force_exit();
                }
            });
        }
    }

    fn read_worker(&self, process: gio::Subprocess, discovery: bool) {
        let stream = gio::DataInputStream::new(&process.stdout_pipe().unwrap());
        let weak = self.downgrade();
        glib::spawn_future_local(async move {
            let mut had_error = false;
            while let Ok(Some(line)) = stream.read_line_future(glib::Priority::DEFAULT).await {
                let line = String::from_utf8_lossy(&line);
                let fields: Vec<_> = line.split('\t').collect();
                if let Some(obj) = weak.upgrade() {
                    had_error |= fields.first() == Some(&"ERROR");
                    obj.worker_event(&fields, discovery);
                } else {
                    process.force_exit();
                    break;
                }
            }
            let _ = process.wait_future().await;
            if let Some(obj) = weak.upgrade() {
                if discovery {
                    obj.imp().session.borrow_mut().discovery.take();
                    obj.ui().spinner.set_visible(false);
                    obj.ui().refresh.set_sensitive(true);
                    obj.ui().scanner.set_subtitle("");
                    if obj.imp().session.borrow().devices.is_empty() && !had_error {
                        obj.show_error(&gettext(
                            "No scanners found. Connect a scanner and refresh.",
                        ));
                    }
                } else {
                    let mut s = obj.imp().session.borrow_mut();
                    s.process.take();
                    s.scanning = false;
                    let stopped = s.stopping;
                    s.stopping = false;
                    drop(s);
                    if !had_error && !stopped && !process.is_successful() {
                        obj.show_error(&gettext(
                            "The scanner disconnected. Completed pages have been kept.",
                        ));
                    }
                }
                obj.update_controls();
                obj.emit_by_name::<()>("changed", &[]);
            }
        });
    }
    fn worker_event(&self, fields: &[&str], discovery: bool) {
        let decoded = |i: usize| {
            fields
                .get(i)
                .and_then(|s| glib::uri_unescape_string(*s, None::<&str>))
                .map(|s| s.to_string())
        };
        match fields.first().copied().unwrap_or("") {
            "DEVICE" if discovery => {
                if let (Some(id), Some(label)) = (decoded(1), decoded(2)) {
                    self.imp()
                        .session
                        .borrow_mut()
                        .devices
                        .push((id, label.clone()));
                    self.ui().devices.append(&label);
                    if self.ui().devices.n_items() == 1 {
                        self.ui().scanner.set_selected(0);
                    }
                    self.ui().status.set_visible(false);
                    self.update_controls();
                }
            }
            "ERROR" => {
                if let Some(text) = decoded(1) {
                    self.show_error(&text);
                }
            }
            "START" => {
                self.ui().progress.set_fraction(0.0);
            }
            "PROGRESS" => {
                if let Some(f) = fields.get(1).and_then(|s| s.parse::<f64>().ok()) {
                    self.ui().progress.set_fraction(f.clamp(0.0, 1.0));
                }
            }
            "SETTINGS" => {
                if let Some(dpi) = fields.get(1).and_then(|s| s.parse::<u32>().ok())
                    && let Some(index) = RESOLUTIONS.iter().position(|v| *v == dpi)
                {
                    if self.ui().kind.selected() == 0 {
                        self.ui().image_dpi.set_selected(index as u32);
                    } else {
                        self.ui().text_dpi.set_selected(index as u32);
                    }
                }
            }
            "WAIT" => {
                if let Some(text) = decoded(1) {
                    self.ui().toast.add_toast(adw::Toast::new(&text));
                }
            }
            "PAGE" => {
                if let Some(path) = decoded(1) {
                    let dpi = fields.get(2).and_then(|s| s.parse().ok()).unwrap_or(300);
                    let mut s = self.imp().session.borrow_mut();
                    s.next_id += 1;
                    let id = s.next_id;
                    s.pages.push(ScanPage {
                        id,
                        path: PathBuf::from(&path),
                        dpi,
                    });
                    s.selected = Some(id);
                    drop(s);
                    self.changed();
                    self.queue_pdf(Change::Append {
                        id,
                        image: PathBuf::from(path),
                        dpi,
                        quality: self.ui().compression.value().round() as u32,
                    });
                }
            }
            _ => {}
        }
    }
    fn selected_page(&self) -> Option<ScanPage> {
        let s = self.imp().session.borrow();
        s.pages.iter().find(|p| Some(p.id) == s.selected).cloned()
    }
    fn affected_ids(&self) -> Vec<u64> {
        let s = self.imp().session.borrow();
        if self
            .ui()
            .actions
            .lookup_action("scope")
            .and_then(|a| a.state())
            .and_then(|v| v.get::<String>())
            .as_deref()
            == Some("document")
        {
            s.pdf_ids.clone()
        } else {
            s.selected.into_iter().collect()
        }
    }
    fn rotate(&self, degrees: i32) {
        let ids = self.affected_ids();
        if ids.is_empty() {
            return;
        }
        let model = self.ui().viewer.model();
        model.set_rotation((model.rotation() + degrees).rem_euclid(360));
        self.changed();
        self.queue_pdf(Change::Rotate { ids, degrees });
    }
    fn select(&self, id: u64) {
        let index = self
            .imp()
            .session
            .borrow()
            .pdf_ids
            .iter()
            .position(|v| *v == id);
        if let Some(index) = index {
            self.imp().session.borrow_mut().selected = Some(id);
            self.ui().viewer.model().set_page(index as i32);
        }
    }
    fn queue_pdf(&self, change: Change) {
        self.imp().session.borrow_mut().queue.push_back(change);
        self.pump_pdf();
    }
    fn pump_pdf(&self) {
        if self.imp().session.borrow().writing {
            return;
        }
        let Some(change) = self.imp().session.borrow_mut().queue.pop_front() else {
            return;
        };
        self.imp().session.borrow_mut().writing = true;
        self.update_controls();
        glib::spawn_future_local(glib::clone!(
            #[weak(rename_to=obj)]
            self,
            async move {
                let result = obj.apply_change(change).await;
                obj.imp().session.borrow_mut().writing = false;
                if let Err(error) = result {
                    obj.show_error(&error);
                }
                obj.update_controls();
                obj.pump_pdf();
            }
        ));
    }
    async fn snapshot(&self) -> Result<Option<PathBuf>, String> {
        let Some(doc) = self.ui().viewer.model().document() else {
            return Ok(None);
        };
        if !doc.is_modified() {
            return Ok(self.imp().session.borrow().pdf.clone());
        }
        let dir = self.ensure_directory()?;
        let path = dir.join(format!("annotated-{}.pdf", glib::uuid_string_random()));
        let uri = gio::File::for_path(&path).uri();
        let original = doc.uri().ok_or("The working PDF has no location")?;
        let job = papers_view::JobSave::new(&doc, &uri, &original);
        let (tx, rx) = futures::channel::oneshot::channel();
        let tx = RefCell::new(Some(tx));
        job.connect_finished(move |job| {
            if let Some(tx) = tx.take() {
                let _ = tx.send(job.is_succeeded().map_err(|e| e.to_string()));
            }
        });
        job.scheduler_push_job(papers_view::JobPriority::PriorityUrgent);
        rx.await.map_err(|e| e.to_string())??;
        self.imp().session.borrow_mut().pdf = Some(path.clone());
        Ok(Some(path))
    }
    fn ensure_directory(&self) -> Result<PathBuf, String> {
        let mut s = self.imp().session.borrow_mut();
        if s.directory.is_none() {
            s.directory = Some(
                tempfile::Builder::new()
                    .prefix("dokument-scan-")
                    .tempdir()
                    .map_err(|e| e.to_string())?
                    .keep(),
            );
        }
        Ok(s.directory.clone().unwrap())
    }
    async fn apply_change(&self, change: Change) -> Result<(), String> {
        let input = self.snapshot().await?;
        let ids = self.imp().session.borrow().pdf_ids.clone();
        let output = self
            .ensure_directory()?
            .join(format!("working-{}.pdf", glib::uuid_string_random()));
        let target = output.clone();
        let replay = change.clone();
        let updated =
            gio::spawn_blocking(move || scan_pdf::apply(input.as_deref(), &target, ids, change))
                .await
                .map_err(|_| gettext("The PDF editor stopped unexpectedly."))??;
        if updated.is_empty() {
            self.ui().viewer.reload_document(None);
            let mut s = self.imp().session.borrow_mut();
            s.pdf = None;
            s.pdf_ids.clear();
            drop(s);
            return Ok(());
        }
        if self
            .ui()
            .viewer
            .model()
            .document()
            .is_some_and(|doc| doc.is_modified())
        {
            self.imp().session.borrow_mut().queue.push_front(replay);
            let _ = std::fs::remove_file(&output);
            return Ok(());
        }
        let first = self.ui().viewer.model().document().is_none();
        let sizing = self.ui().viewer.model().sizing_mode();
        let scale = self.ui().viewer.model().scale();
        let continuous = self.ui().viewer.model().is_continuous();
        let doc = Self::load_pdf(&output).await?;
        let selected = self.imp().session.borrow().selected;
        self.imp().session.borrow_mut().pdf = Some(output);
        self.imp().session.borrow_mut().pdf_ids = updated;
        self.ui()
            .viewer
            .open_document(&doc, None, None, WindowRunMode::Normal);
        let model = self.ui().viewer.model();
        model.set_sizing_mode(if first {
            papers_view::SizingMode::FitPage
        } else {
            sizing
        });
        model.set_scale(scale);
        model.set_continuous(!first && continuous);
        model.set_rotation(0);
        if let Some(selected) = selected {
            self.select(selected);
        }
        self.cleanup_pdfs();
        Ok(())
    }
    fn cleanup_pdfs(&self) {
        let state = self.imp().session.borrow();
        if state.exporting || state.writing && !state.queue.is_empty() {
            return;
        }
        let Some(directory) = state.directory.as_ref() else {
            return;
        };
        let document_path = self
            .ui()
            .viewer
            .model()
            .document()
            .and_then(|d| d.uri())
            .and_then(|u| gio::File::for_uri(&u).path());
        if let Ok(entries) = std::fs::read_dir(directory) {
            for entry in entries.flatten() {
                let path = entry.path();
                let name = entry.file_name();
                let name = name.to_string_lossy();
                if (name.starts_with("working-") || name.starts_with("annotated-"))
                    && name.ends_with(".pdf")
                    && Some(&path) != state.pdf.as_ref()
                    && Some(&path) != document_path.as_ref()
                {
                    let _ = std::fs::remove_file(path);
                }
            }
        }
    }
    async fn load_pdf(path: &std::path::Path) -> Result<Document, String> {
        let job = papers_view::JobLoad::new();
        job.set_uri(&gio::File::for_path(path).uri());
        let (tx, rx) = futures::channel::oneshot::channel();
        let tx = RefCell::new(Some(tx));
        job.connect_finished(move |job| {
            if let Some(tx) = tx.take() {
                let result = job
                    .is_succeeded()
                    .map_err(|e| e.to_string())
                    .and_then(|()| {
                        job.loaded_document()
                            .ok_or_else(|| "The PDF could not be loaded".into())
                    });
                let _ = tx.send(result);
            }
        });
        job.scheduler_push_job(papers_view::JobPriority::PriorityUrgent);
        rx.await.map_err(|e| e.to_string())?
    }
    fn reorder(&self, id: u64, before: Option<u64>) {
        if before == Some(id) {
            return;
        }
        let mut s = self.imp().session.borrow_mut();
        let Some(from) = s.pages.iter().position(|p| p.id == id) else {
            return;
        };
        let page = s.pages.remove(from);
        let to = before
            .and_then(|v| s.pages.iter().position(|p| p.id == v))
            .unwrap_or(s.pages.len());
        s.pages.insert(to, page);
        let order = s.pages.iter().map(|p| p.id).collect();
        drop(s);
        self.changed();
        self.queue_pdf(Change::Order(order));
    }
    fn remove_page(&self, id: u64) {
        let mut s = self.imp().session.borrow_mut();
        s.pages.retain(|p| p.id != id);
        if s.selected == Some(id) {
            s.selected = s.pages.last().map(|p| p.id);
        }
        let order = s.pages.iter().map(|p| p.id).collect();
        drop(s);
        self.changed();
        self.queue_pdf(Change::Order(order));
    }
    fn move_selected(&self, later: bool) {
        let state = self.imp().session.borrow();
        let Some(index) = state
            .pages
            .iter()
            .position(|p| Some(p.id) == state.selected)
        else {
            return;
        };
        let id = state.pages[index].id;
        let before = if later {
            state.pages.get(index + 2).map(|p| p.id)
        } else {
            index
                .checked_sub(1)
                .and_then(|i| state.pages.get(i))
                .map(|p| p.id)
        };
        if !later && index == 0 {
            return;
        }
        drop(state);
        self.reorder(id, before);
    }
    fn show_preferences(&self) {
        self.ui().viewer.close_document_menu();
        glib::idle_add_local_once(glib::clone!(
            #[weak(rename_to=obj)]
            self,
            move || {
                obj.ui().preferences_layer.set_visible(true);
                obj.ui()
                    .preferences
                    .child_focus(gtk::DirectionType::TabForward);
            }
        ));
    }
    fn close_preferences(&self) {
        self.ui().preferences_layer.set_visible(false);
        self.ui().viewer.focus_view();
    }
    fn show_crop(&self) {
        let Some(page) = self.selected_page() else {
            return;
        };
        let Some(pdf) = self.imp().session.borrow().pdf.clone() else {
            return;
        };
        let Some(index) = self
            .imp()
            .session
            .borrow()
            .pdf_ids
            .iter()
            .position(|v| *v == page.id)
        else {
            return;
        };
        let affected = self.affected_ids();
        glib::spawn_future_local(glib::clone!(
            #[weak(rename_to=obj)]
            self,
            async move {
                let result = gio::spawn_blocking(move || {
                    let (bounds, rotation) = scan_pdf::geometry(&pdf, index)?;
                    let preview = scan_document::preview_rotated(&page, 1200, rotation)?;
                    let (mut w, mut h) = (bounds[2] - bounds[0], bounds[3] - bounds[1]);
                    if rotation % 180 != 0 {
                        std::mem::swap(&mut w, &mut h);
                    }
                    Ok::<_, String>((preview, w, h))
                })
                .await;
                match result {
                    Ok(Ok(((bytes, w, h), width, height))) => {
                        obj.crop_dialog(bytes, w, h, width, height, affected)
                    }
                    Ok(Err(e)) => obj.show_error(&e),
                    Err(_) => obj.show_error(&gettext("Could not prepare the crop preview.")),
                }
            }
        ));
    }
    fn crop_dialog(&self, bytes: Vec<u8>, w: u32, h: u32, width: f64, height: f64, ids: Vec<u64>) {
        let texture = gdk::MemoryTexture::new(
            w as i32,
            h as i32,
            gdk::MemoryFormat::R8g8b8,
            &glib::Bytes::from_owned(bytes),
            w as usize * 3,
        );
        let picture = gtk::Picture::builder()
            .paintable(&texture)
            .can_shrink(true)
            .content_fit(gtk::ContentFit::Contain)
            .build();
        let overlay = gtk::Overlay::new();
        overlay.set_child(Some(&picture));
        let area = gtk::DrawingArea::builder()
            .hexpand(true)
            .vexpand(true)
            .build();
        area.set_cursor_from_name(Some("crosshair"));
        overlay.add_overlay(&area);
        let selection = std::rc::Rc::new(Cell::new((0.0f64, 0.0f64, 1.0f64, 1.0f64)));
        let origin = std::rc::Rc::new(Cell::new((0.0, 0.0)));
        let rect = move |area: &gtk::DrawingArea| {
            let scale = (area.width() as f64 / w as f64).min(area.height() as f64 / h as f64);
            let (sw, sh) = (w as f64 * scale, h as f64 * scale);
            (
                (area.width() as f64 - sw) / 2.0,
                (area.height() as f64 - sh) / 2.0,
                sw,
                sh,
            )
        };
        area.set_draw_func(glib::clone!(
            #[strong]
            selection,
            move |area, cr, _, _| {
                let (x, y, w, h) = rect(area);
                let (l, t, r, b) = selection.get();
                cr.set_source_rgba(0.0, 0.0, 0.0, 0.4);
                cr.rectangle(x, y, w, h);
                cr.rectangle(x + l * w, y + t * h, (r - l) * w, (b - t) * h);
                cr.set_fill_rule(cairo::FillRule::EvenOdd);
                let _ = cr.fill();
                cr.set_source_rgb(1.0, 1.0, 1.0);
                cr.set_line_width(2.0);
                cr.rectangle(x + l * w, y + t * h, (r - l) * w, (b - t) * h);
                let _ = cr.stroke();
            }
        ));
        let drag = gtk::GestureDrag::new();
        drag.connect_drag_begin(glib::clone!(
            #[strong]
            origin,
            move |_, x, y| origin.set((x, y))
        ));
        drag.connect_drag_update(glib::clone!(
            #[weak]
            area,
            #[strong]
            origin,
            #[strong]
            selection,
            move |_, dx, dy| {
                let (x, y, w, h) = rect(&area);
                let (sx, sy) = origin.get();
                let l = ((sx.min(sx + dx) - x) / w).clamp(0.0, 1.0);
                let t = ((sy.min(sy + dy) - y) / h).clamp(0.0, 1.0);
                let r = ((sx.max(sx + dx) - x) / w).clamp(0.0, 1.0);
                let b = ((sy.max(sy + dy) - y) / h).clamp(0.0, 1.0);
                if r - l > 0.005 && b - t > 0.005 {
                    selection.set((l, t, r, b));
                    area.queue_draw();
                }
            }
        ));
        // A manual drag switches from a physical paper preset to a custom crop.
        let crop_drag = drag.clone();
        area.add_controller(drag);
        let frame = gtk::AspectFrame::builder()
            .ratio(w as f32 / h as f32)
            .obey_child(false)
            .height_request(360)
            .child(&overlay)
            .build();
        let formats = gtk::DropDown::from_strings(&[
            &gettext("Custom"),
            &gettext("Original Size"),
            "A6",
            "A5",
            "A4",
            "A3",
            &gettext("Letter"),
            &gettext("Legal"),
            "4×6",
        ]);
        crop_drag.connect_drag_begin(glib::clone!(
            #[weak]
            formats,
            move |_, _, _| formats.set_selected(0)
        ));
        formats.connect_selected_notify(glib::clone!(
            #[weak]
            area,
            #[strong]
            selection,
            move |dropdown| {
                let mm = match dropdown.selected() {
                    2 => (105.0, 148.0),
                    3 => (148.0, 210.0),
                    4 => (210.0, 297.0),
                    5 => (297.0, 420.0),
                    6 => (215.9, 279.4),
                    7 => (215.9, 355.6),
                    8 => (101.6, 152.4),
                    _ => {
                        selection.set((0.0, 0.0, 1.0, 1.0));
                        area.queue_draw();
                        return;
                    }
                };
                let rw = (mm.0 * 72.0 / 25.4 / width).min(1.0);
                let rh = (mm.1 * 72.0 / 25.4 / height).min(1.0);
                selection.set((
                    (1.0 - rw) / 2.0,
                    (1.0 - rh) / 2.0,
                    (1.0 + rw) / 2.0,
                    (1.0 + rh) / 2.0,
                ));
                area.queue_draw();
            }
        ));
        let body = gtk::Box::builder()
            .orientation(gtk::Orientation::Vertical)
            .spacing(12)
            .margin_start(16)
            .margin_end(16)
            .margin_bottom(16)
            .build();
        body.append(&frame);
        body.append(&formats);
        let toolbar = adw::ToolbarView::new();
        let header = adw::HeaderBar::new();
        header.set_show_end_title_buttons(false);
        header.set_show_start_title_buttons(false);
        header.set_title_widget(Some(&adw::WindowTitle::new(&gettext("Crop"), "")));
        let cancel = gtk::Button::with_label(&gettext("Cancel"));
        let apply = gtk::Button::with_label(&gettext("Apply"));
        apply.add_css_class("suggested-action");
        header.pack_start(&cancel);
        header.pack_end(&apply);
        toolbar.add_top_bar(&header);
        toolbar.set_content(Some(&body));
        let dialog = adw::Dialog::builder()
            .content_width(650)
            .content_height(650)
            .child(&toolbar)
            .build();
        cancel.connect_clicked(glib::clone!(
            #[weak]
            dialog,
            move |_| {
                dialog.close();
            }
        ));
        apply.connect_clicked(glib::clone!(
            #[weak(rename_to=obj)]
            self,
            #[weak]
            dialog,
            #[strong]
            selection,
            #[weak]
            formats,
            move |_| {
                let size = match formats.selected() {
                    2 => Some((105.0, 148.0)),
                    3 => Some((148.0, 210.0)),
                    4 => Some((210.0, 297.0)),
                    5 => Some((297.0, 420.0)),
                    6 => Some((215.9, 279.4)),
                    7 => Some((215.9, 355.6)),
                    8 => Some((101.6, 152.4)),
                    _ => None,
                }
                .map(|(w, h)| (w * 72.0 / 25.4, h * 72.0 / 25.4));
                obj.changed();
                obj.queue_pdf(Change::Crop {
                    ids: ids.clone(),
                    bounds: selection.get(),
                    size,
                });
                dialog.close();
            }
        ));
        dialog.present(Some(self));
    }
    async fn make_pdf(&self) -> Result<PathBuf, String> {
        while self.imp().session.borrow().writing || !self.imp().session.borrow().queue.is_empty() {
            glib::timeout_future(Duration::from_millis(10)).await;
        }
        self.snapshot()
            .await?
            .ok_or_else(|| gettext("There are no scanned pages to save."))
    }
    async fn save_pdf(&self) -> bool {
        if self.imp().session.borrow().exporting {
            return false;
        }
        let dialog = gtk::FileDialog::builder()
            .title(gettext("Save Scanned Document"))
            .initial_name("Scan.pdf")
            .modal(true)
            .build();
        let filter = gtk::FileFilter::new();
        filter.set_name(Some(&gettext("PDF Documents")));
        filter.add_mime_type("application/pdf");
        let filters = gio::ListStore::new::<gtk::FileFilter>();
        filters.append(&filter);
        dialog.set_filters(Some(&filters));
        dialog.set_default_filter(Some(&filter));
        let parent = self.root().and_downcast::<gtk::Window>();
        let Ok(destination) = dialog.save_future(parent.as_ref()).await else {
            return false;
        };
        let revision = self.imp().session.borrow().revision;

        match self.make_pdf().await {
            Ok(path) => {
                self.imp().session.borrow_mut().exporting = true;
                self.update_controls();
                let result = gio::File::for_path(path)
                    .copy_future(
                        &destination,
                        gio::FileCopyFlags::OVERWRITE,
                        glib::Priority::DEFAULT,
                    )
                    .0
                    .await;
                self.imp().session.borrow_mut().exporting = false;
                self.update_controls();
                if let Err(e) = result {
                    self.show_error(&e.to_string());
                    return false;
                }
                if self.imp().session.borrow().revision == revision {
                    self.imp().session.borrow_mut().dirty = false;
                }
                self.emit_by_name::<()>("changed", &[]);

                true
            }
            Err(e) => {
                self.show_error(&e);
                false
            }
        }
    }

    fn share(&self, email: bool) {
        if self.imp().session.borrow().exporting {
            return;
        }
        glib::spawn_future_local(glib::clone!(
            #[weak(rename_to = obj)]
            self,
            async move {
                if email {
                    obj.imp().session.borrow_mut().exporting = true;
                    obj.update_controls();
                    match obj.make_pdf().await {
                        Ok(path) => {
                            let result = async {
                                // A draft attachment must outlive closing the scan tab.
                                let cache = glib::user_cache_dir().join("dokument").join("email");
                                std::fs::create_dir_all(&cache).map_err(|e| e.to_string())?;
                                let attachment =
                                    cache.join(format!("Scan-{}.pdf", glib::uuid_string_random()));
                                gio::File::for_path(path)
                                    .copy_future(
                                        &gio::File::for_path(&attachment),
                                        gio::FileCopyFlags::NONE,
                                        glib::Priority::DEFAULT,
                                    )
                                    .0
                                    .await
                                    .map_err(|e| e.to_string())?;
                                let file =
                                    std::fs::File::open(attachment).map_err(|e| e.to_string())?;
                                ashpd::desktop::email::EmailRequest::default()
                                    .subject(gettext("Scanned Document").as_str())
                                    .attach(file.into())
                                    .send()
                                    .await
                                    .map_err(|e| e.to_string())?
                                    .response()
                                    .map_err(|e| e.to_string())
                            }
                            .await;
                            match result {
                                Ok(()) => {}
                                Err(e) => {
                                    warn!("Unable to compose scan email: {e}");
                                    obj.show_error(&gettext("Could not open an email composer. Set a default email application and try again."));
                                }
                            }
                        }
                        Err(e) => obj.show_error(&e),
                    }
                    obj.imp().session.borrow_mut().exporting = false;
                    obj.update_controls();
                    obj.cleanup_pdfs();
                } else {
                    let _ = obj.ui().viewer.activate_action("doc.print", None);
                }
            }
        ));
    }

    pub fn confirm_leave(&self, back: bool) {
        if self.imp().closing.replace(true) {
            return;
        }
        if !self.dirty() && !self.busy() {
            self.imp().closing.set(false);
            self.emit_by_name::<()>(
                if back { "back" } else { "close-finished" },
                if back { &[] } else { &[&true] },
            );
            return;
        }
        let dialog = adw::AlertDialog::builder()
            .heading(gettext("Keep Your Scanned Pages?"))
            .body(gettext(
                "Save your scanned pages as a PDF before closing this scan.",
            ))
            .close_response("cancel")
            .default_response("cancel")
            .build();
        dialog.add_responses(&[
            ("cancel", &gettext("Cancel")),
            ("discard", &gettext("Discard")),
            ("save", &gettext("Save PDF")),
        ]);
        dialog.set_response_appearance("discard", adw::ResponseAppearance::Destructive);
        dialog.set_response_appearance("save", adw::ResponseAppearance::Suggested);
        dialog.set_response_enabled(
            "save",
            !self.busy() && !self.imp().session.borrow().pages.is_empty(),
        );
        glib::spawn_future_local(glib::clone!(
            #[weak(rename_to = obj)]
            self,
            async move {
                let response = dialog.choose_future(Some(&obj)).await;
                let allowed = response == "discard" || (response == "save" && obj.save_pdf().await);
                obj.imp().closing.set(false);
                if allowed {
                    obj.stop();
                }
                if back {
                    if allowed {
                        obj.emit_by_name::<()>("back", &[]);
                    }
                } else {
                    obj.emit_by_name::<()>("close-finished", &[&allowed]);
                }
            }
        ));
    }
}
