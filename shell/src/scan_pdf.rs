// SPDX-License-Identifier: GPL-2.0-or-later
//! Edit page dictionaries rather than decoding and recompressing scanned pixels.
use lopdf::{Document, Object, ObjectId, dictionary};
use std::path::{Path, PathBuf};

#[derive(Clone, Debug)]
pub enum Change {
    Append {
        id: u64,
        image: PathBuf,
        dpi: u32,
        quality: u32,
    },
    Rotate {
        ids: Vec<u64>,
        degrees: i32,
    },
    Crop {
        ids: Vec<u64>,
        bounds: (f64, f64, f64, f64),
        size: Option<(f64, f64)>,
    },
    Order(Vec<u64>),
}

fn number(value: &Object) -> Result<f64, String> {
    value
        .as_float()
        .map(|v| v as f64)
        .map_err(|e| e.to_string())
}

pub fn geometry(path: &Path, index: usize) -> Result<([f64; 4], i32), String> {
    let doc = Document::load(path).map_err(|e| e.to_string())?;
    geometry_in(&doc, doc.get_pages()[&(index as u32 + 1)])
}

fn inherited(doc: &Document, mut id: ObjectId, name: &[u8]) -> Option<Object> {
    for _ in 0..128 {
        let dict = doc.get_dictionary(id).ok()?;
        if let Ok(value) = dict.get(name) {
            return doc.dereference(value).ok().map(|(_, value)| value.clone());
        }
        id = dict.get(b"Parent").and_then(Object::as_reference).ok()?;
    }
    None
}
fn flatten_pages(doc: &mut Document) -> Result<(), String> {
    for id in doc.get_pages().into_values() {
        for key in [b"MediaBox".as_slice(), b"Resources", b"Rotate", b"CropBox"] {
            if let Some(value) = inherited(doc, id, key) {
                doc.get_dictionary_mut(id)
                    .map_err(|e| e.to_string())?
                    .set(key, value);
            }
        }
    }
    Ok(())
}
fn geometry_in(doc: &Document, id: ObjectId) -> Result<([f64; 4], i32), String> {
    let media = inherited(doc, id, b"MediaBox").ok_or("The PDF page has no original size")?;
    let a = media.as_array().map_err(|e| e.to_string())?;
    let bounds = [
        number(&a[0])?,
        number(&a[1])?,
        number(&a[2])?,
        number(&a[3])?,
    ];
    let rotation = inherited(doc, id, b"Rotate")
        .and_then(|v| v.as_i64().ok())
        .unwrap_or(0) as i32;
    Ok((bounds, rotation.rem_euclid(360)))
}

fn rewrite_pages(doc: &mut Document, ids: &[ObjectId]) -> Result<(), String> {
    let parent = doc.add_object(dictionary! { "Type" => "Pages", "Kids" => ids.iter().copied().map(Object::Reference).collect::<Vec<_>>(), "Count" => ids.len() as i64 });
    for id in ids {
        doc.get_dictionary_mut(*id)
            .map_err(|e| e.to_string())?
            .set("Parent", parent);
    }
    doc.catalog_mut()
        .map_err(|e| e.to_string())?
        .set("Pages", parent);
    Ok(())
}

pub fn apply(
    input: Option<&Path>,
    output: &Path,
    mut ids: Vec<u64>,
    change: Change,
) -> Result<Vec<u64>, String> {
    let mut doc = if let Some(path) = input {
        Document::load(path).map_err(|e| e.to_string())?
    } else {
        let mut doc = Document::with_version("1.7");
        let root = doc.add_object(dictionary! { "Type" => "Catalog" });
        doc.trailer.set("Root", root);
        doc
    };
    flatten_pages(&mut doc)?;
    let mut pages: Vec<ObjectId> = doc.get_pages().into_values().collect();
    match change {
        Change::Append {
            id,
            image,
            dpi,
            quality,
        } => {
            let single = output.with_extension("new-page.pdf");
            let page = crate::scan_document::ScanPage {
                id,
                path: image,
                dpi,
            };
            // Encode each acquired image only once. All subsequent edits retain its stream.
            crate::scan_document::export_pdf(std::slice::from_ref(&page), &single, quality)?;
            let mut addition = Document::load(&single).map_err(|e| e.to_string())?;
            flatten_pages(&mut addition)?;
            addition.renumber_objects_with(doc.max_id + 1);
            pages.extend(addition.get_pages().into_values());
            doc.max_id = addition.max_id;
            doc.objects.extend(addition.objects);
            ids.push(id);
            let _ = std::fs::remove_file(single);
        }
        Change::Rotate {
            ids: affected,
            degrees,
        } => {
            for (id, page) in ids.iter().zip(&pages) {
                if affected.contains(id) {
                    let (_, rotation) = geometry_in(&doc, *page)?;
                    doc.get_dictionary_mut(*page)
                        .map_err(|e| e.to_string())?
                        .set("Rotate", (rotation + degrees).rem_euclid(360));
                }
            }
        }
        Change::Crop {
            ids: affected,
            bounds,
            size,
        } => {
            for (id, page) in ids.iter().zip(&pages) {
                if !affected.contains(id) {
                    continue;
                }
                let ([x, y, r, t], rotation) = geometry_in(&doc, *page)?;
                let (left, top, right, bottom) = if let Some((width, height)) = size {
                    let (w, h) = if rotation % 180 == 0 {
                        (r - x, t - y)
                    } else {
                        (t - y, r - x)
                    };
                    let rw = (width / w).min(1.0);
                    let rh = (height / h).min(1.0);
                    (
                        (1.0 - rw) / 2.0,
                        (1.0 - rh) / 2.0,
                        (1.0 + rw) / 2.0,
                        (1.0 + rh) / 2.0,
                    )
                } else {
                    bounds
                };
                // Normalized coordinates describe the original page as currently rotated.
                let (l, u, rr, b) = match rotation {
                    90 => (top, 1.0 - right, bottom, 1.0 - left),
                    180 => (1.0 - right, 1.0 - bottom, 1.0 - left, 1.0 - top),
                    270 => (1.0 - bottom, left, 1.0 - top, right),
                    _ => (left, top, right, bottom),
                };
                let rect = vec![
                    Object::Real((x + l * (r - x)) as f32),
                    Object::Real((t - b * (t - y)) as f32),
                    Object::Real((x + rr * (r - x)) as f32),
                    Object::Real((t - u * (t - y)) as f32),
                ];
                doc.get_dictionary_mut(*page)
                    .map_err(|e| e.to_string())?
                    .set("CropBox", rect);
            }
        }
        Change::Order(order) => {
            let old = ids.clone();
            pages = order
                .iter()
                .filter_map(|id| old.iter().position(|v| v == id).map(|i| pages[i]))
                .collect();
            ids = order;
        }
    }
    rewrite_pages(&mut doc, &pages)?;
    doc.prune_objects();
    doc.save(output).map_err(|e| e.to_string())?;
    Ok(ids)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn recropping_uses_original_geometry_and_keeps_image_streams() {
        let dir = tempfile::tempdir().unwrap();
        let image = dir.path().join("scan.png");
        image::RgbImage::from_pixel(300, 200, image::Rgb([100, 200, 255]))
            .save(&image)
            .unwrap();
        let first = dir.path().join("one.pdf");
        apply(
            None,
            &first,
            vec![],
            Change::Append {
                id: 1,
                image,
                dpi: 100,
                quality: 75,
            },
        )
        .unwrap();
        let original = Document::load(&first).unwrap();
        let stream = original
            .objects
            .iter()
            .find_map(|(id, obj)| {
                obj.as_stream()
                    .ok()
                    .filter(|s| {
                        s.dict.get(b"Subtype").and_then(Object::as_name).ok() == Some(b"Image")
                    })
                    .map(|stream| (*id, stream.content.clone()))
            })
            .unwrap();
        let small = dir.path().join("small.pdf");
        apply(
            Some(&first),
            &small,
            vec![1],
            Change::Crop {
                ids: vec![1],
                bounds: (0.25, 0.25, 0.75, 0.75),
                size: None,
            },
        )
        .unwrap();
        let large = dir.path().join("large.pdf");
        apply(
            Some(&small),
            &large,
            vec![1],
            Change::Crop {
                ids: vec![1],
                bounds: (0.0, 0.0, 1.0, 1.0),
                size: None,
            },
        )
        .unwrap();
        let doc = Document::load(&large).unwrap();
        let id = doc.get_pages()[&1];
        let page = doc.get_dictionary(id).unwrap();
        let crop = page.get(b"CropBox").unwrap().as_array().unwrap();
        let media = page.get(b"MediaBox").unwrap().as_array().unwrap();
        for (a, b) in crop.iter().zip(media) {
            assert!((number(a).unwrap() - number(b).unwrap()).abs() < 0.001);
        }
        assert_eq!(
            doc.get_object(stream.0)
                .unwrap()
                .as_stream()
                .unwrap()
                .content,
            stream.1
        );
    }
    #[test]
    fn physical_crop_presets_use_each_pages_original_size() {
        let dir = tempfile::tempdir().unwrap();
        let image = dir.path().join("scan.png");
        image::RgbImage::from_pixel(240, 300, image::Rgb([100, 200, 255]))
            .save(&image)
            .unwrap();
        let one = dir.path().join("one.pdf");
        let two = dir.path().join("two.pdf");
        let cropped = dir.path().join("cropped.pdf");
        apply(
            None,
            &one,
            vec![],
            Change::Append {
                id: 1,
                image: image.clone(),
                dpi: 50,
                quality: 75,
            },
        )
        .unwrap();
        apply(
            Some(&one),
            &two,
            vec![1],
            Change::Append {
                id: 2,
                image,
                dpi: 100,
                quality: 75,
            },
        )
        .unwrap();
        apply(
            Some(&two),
            &cropped,
            vec![1, 2],
            Change::Crop {
                ids: vec![1, 2],
                bounds: (0.0, 0.0, 1.0, 1.0),
                size: Some((72.0, 100.0)),
            },
        )
        .unwrap();
        let doc = Document::load(cropped).unwrap();
        for id in doc.get_pages().into_values() {
            let a = doc
                .get_dictionary(id)
                .unwrap()
                .get(b"CropBox")
                .unwrap()
                .as_array()
                .unwrap();
            assert!((number(&a[2]).unwrap() - number(&a[0]).unwrap() - 72.0).abs() < 0.001);
            assert!((number(&a[3]).unwrap() - number(&a[1]).unwrap() - 100.0).abs() < 0.001);
        }
    }
}
