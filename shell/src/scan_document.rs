// SPDX-License-Identifier: GPL-2.0-or-later
use image::DynamicImage;
use std::path::{Path, PathBuf};

#[derive(Clone, Debug)]
pub struct ScanPage {
    pub id: u64,
    pub path: PathBuf,
    pub dpi: u32,
}

impl ScanPage {
    pub fn image(&self) -> Result<DynamicImage, String> {
        image::open(&self.path).map_err(|e| e.to_string())
    }
}

pub fn preview_rotated(
    page: &ScanPage,
    size: u32,
    rotation: i32,
) -> Result<(Vec<u8>, u32, u32), String> {
    let cached = page.path.with_extension("preview.png");
    let mut image = if cached.exists() {
        image::open(&cached).map_err(|e| e.to_string())?
    } else {
        let image = image::open(&page.path)
            .map_err(|e| e.to_string())?
            .thumbnail(1200, 1200);
        let temp = cached.with_extension(format!("{}.png", gtk::glib::uuid_string_random()));
        if image.save(&temp).is_ok() {
            let _ = std::fs::rename(temp, &cached);
        }
        image
    };
    image = match rotation.rem_euclid(360) {
        90 => image.rotate90(),
        180 => image.rotate180(),
        270 => image.rotate270(),
        _ => image,
    };
    let image = image.thumbnail(size, size).to_rgb8();
    let (w, h) = image.dimensions();
    Ok((image.into_raw(), w, h))
}

pub fn surface(image: &DynamicImage, compression: u32) -> Result<cairo::ImageSurface, String> {
    let rgb = image.to_rgb8();
    let (w, h) = rgb.dimensions();
    let stride = cairo::Format::Rgb24
        .stride_for_width(w)
        .map_err(|e| e.to_string())?;
    let mut data = vec![0; stride as usize * h as usize];
    for (x, y, p) in rgb.enumerate_pixels() {
        let pixel = 0xff00_0000u32 | (p[0] as u32) << 16 | (p[1] as u32) << 8 | p[2] as u32;
        let offset = y as usize * stride as usize + x as usize * 4;
        data[offset..offset + 4].copy_from_slice(&pixel.to_ne_bytes());
    }
    let surface = cairo::ImageSurface::create_for_data(
        data,
        cairo::Format::Rgb24,
        w as i32,
        h as i32,
        stride,
    )
    .map_err(|e| e.to_string())?;
    if compression < 100 {
        let quality = compression.clamp(1, 100) as u8;
        let mut jpeg = Vec::new();
        image::codecs::jpeg::JpegEncoder::new_with_quality(&mut jpeg, quality)
            .encode_image(&DynamicImage::ImageRgb8(rgb))
            .map_err(|e| e.to_string())?;
        surface
            .set_mime_data(cairo::MIME_TYPE_JPEG, jpeg)
            .map_err(|e| e.to_string())?;
    }
    Ok(surface)
}

pub fn export_pdf(pages: &[ScanPage], path: &Path, compression: u32) -> Result<(), String> {
    if pages.is_empty() {
        return Err("There are no scanned pages to save.".into());
    }
    let pdf = cairo::PdfSurface::new(595.0, 842.0, path).map_err(|e| e.to_string())?;
    let context = cairo::Context::new(&pdf).map_err(|e| e.to_string())?;
    for page in pages {
        let image = page.image()?;
        let cached = page.path.with_extension("preview.png");
        if !cached.exists() {
            let _ = image.thumbnail(1200, 1200).save(cached);
        }
        let scale = 72.0 / page.dpi.max(1) as f64;
        pdf.set_size(image.width() as f64 * scale, image.height() as f64 * scale)
            .map_err(|e| e.to_string())?;
        let image = surface(&image, compression)?;
        context.save().map_err(|e| e.to_string())?;
        context.scale(scale, scale);
        context
            .set_source_surface(&image, 0.0, 0.0)
            .map_err(|e| e.to_string())?;
        context.paint().map_err(|e| e.to_string())?;
        context.restore().map_err(|e| e.to_string())?;
        context.show_page().map_err(|e| e.to_string())?;
    }
    pdf.finish();
    pdf.status().map_err(|e| e.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pdf_export_preserves_physical_size_and_quality_choices() {
        use gtk::prelude::*;
        use papers_document::prelude::*;
        crate::tests::run(|| {
            let dir = tempfile::tempdir().unwrap();
            let source = dir.path().join("original.png");
            image::RgbImage::from_pixel(200, 100, image::Rgb([255, 0, 0]))
                .save(&source)
                .unwrap();
            let page = ScanPage {
                id: 1,
                path: source,
                dpi: 100,
            };
            for quality in [0, 60, 75, 90, 100] {
                let pdf = dir.path().join(format!("export-{quality}.pdf"));
                export_pdf(&[page.clone(), page.clone()], &pdf, quality).unwrap();
                let uri = gtk::gio::File::for_path(&pdf).uri();
                let document = papers_document::Document::factory_get_document(&uri).unwrap();
                papers_document::Document::load(&document, &uri).unwrap();
                assert_eq!(document.n_pages(), 2);
                let (w, h) = document.page_size_uncached(&document.page(0).unwrap());
                assert!((w - 144.0).abs() < 0.001 && (h - 72.0).abs() < 0.001);
            }
        });
    }
}
