// SPDX-FileCopyrightText: 2026 Jonas Sattler
// SPDX-License-Identifier: BSD-3-Clause

//! A plotcore figure in egui.
//!
//! [`PlotView`] does what a host does and nothing more: it tells the figure
//! its size and how egui's fonts measure, hands it egui's pointer and wheel
//! events, and paints the [`Scene`] that comes back. Nothing here decides
//! where a tick goes or what a gesture does, which is what makes this the
//! plot the Qt and Python hosts draw.
//!
//! A line is painted as plotcore's own triangles ([`Strokes::Triangles`]), the
//! stroke the C++ library builds for a host that fills them -- or as a
//! polyline egui feathers itself ([`Strokes::Feathered`]), which antialiases a
//! hairline where a mesh cannot.
//!
//! A scatter is its markers, each a circle egui feathers, in its own colour --
//! or, when it is dense, the image of the pane plotcore painted, which is
//! uploaded into a texture kept from frame to frame ([`Pictures`]) and drawn
//! on the pane's device pixels.
//!
//! One rule about where a scene is made: plotcore measures text by calling
//! back into egui's fonts, which takes the context's lock, so the figure is
//! never asked for a scene from inside `ui.input(...)` or any other closure
//! that holds it.

use egui::epaint::{Mesh, TextShape};
use egui::{
    Color32, ColorImage, CornerRadius, Event, FontId, MouseWheelUnit, Painter, PointerButton, Pos2,
    Rect, Response, Sense, Shape, Stroke, StrokeKind, TextureHandle, TextureId, TextureOptions, Ui,
    Vec2,
};
use plotcore::{Align, Button, Figure, Image, Mark, MarkKind, Modifiers, Rgba, Scene, TextRole};

/// The textures a scene's images are painted from, kept from frame to frame:
/// a scatter's image is uploaded into the texture it had last frame rather than
/// into a new one, and a texture is let go of only with this.
#[derive(Default)]
pub struct Pictures {
    textures: Vec<TextureHandle>,
}

impl Pictures {
    /// Image `slot` of a scene, uploaded.
    fn texture(&mut self, ctx: &egui::Context, slot: usize, image: &Image) -> TextureId {
        let pixels = ColorImage::from_rgba_premultiplied([image.width, image.height], &image.rgba);
        // On the pane's own device pixels, so each texel is one: nothing to blend.
        let options = TextureOptions::NEAREST;
        if let Some(handle) = self.textures.get_mut(slot) {
            handle.set(pixels, options);
            return handle.id();
        }
        let handle = ctx.load_texture(format!("plotcore image {slot}"), pixels, options);
        let id = handle.id();
        self.textures.push(handle);
        id
    }
}

/// How a line's stroke is painted.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Strokes {
    /// plotcore's triangles, as an egui mesh.
    #[default]
    Triangles,
    /// The runs as polylines, feathered by egui's tessellator.
    Feathered,
}

/// A figure, its input state and its last frame, as one egui widget.
pub struct PlotView {
    pub figure: Figure,
    pub strokes: Strokes,
    last: Option<Scene>,
    held: Option<Button>,
    inside: bool,
    measured: Option<[f32; ROLES]>,
    changed: bool,
    pictures: Pictures,
    selection: u64,
}

/// What a wheel notch is in egui's units, as Qt counts it: 120 eighths of a
/// degree. egui reports a notch as one line, or as this many points.
const POINTS_PER_NOTCH: f32 = 50.0;

pub fn colour(c: Rgba) -> Color32 {
    let byte = |v: f64| (v.clamp(0.0, 1.0) * 255.0).round() as u8;
    Color32::from_rgba_unmultiplied(byte(c.red), byte(c.green), byte(c.blue), byte(c.alpha))
}

fn modifiers(m: egui::Modifiers) -> Modifiers {
    let mut out = Modifiers::NONE;
    if m.shift {
        out |= Modifiers::SHIFT;
    }
    if m.ctrl || m.mac_cmd {
        out |= Modifiers::CONTROL;
    }
    if m.alt {
        out |= Modifiers::ALT;
    }
    out
}

fn button(b: PointerButton) -> Option<Button> {
    match b {
        PointerButton::Primary => Some(Button::Left),
        PointerButton::Middle => Some(Button::Middle),
        PointerButton::Secondary => Some(Button::Right),
        _ => None,
    }
}

/// How many faces a scene is set in.
const ROLES: usize = TextRole::ALL.len();

fn role_slot(role: TextRole) -> usize {
    TextRole::ALL.iter().position(|r| *r == role).unwrap_or(0)
}

impl PlotView {
    pub fn new(figure: Figure) -> Self {
        Self {
            figure,
            strokes: Strokes::default(),
            last: None,
            held: None,
            inside: false,
            measured: None,
            changed: false,
            pictures: Pictures::default(),
            selection: 0,
        }
    }

    /// Whether a click or a band changed what is selected of a scatter since
    /// this was last asked.
    pub fn selection_changed(&mut self) -> bool {
        let version = self.figure.selection_version();
        let changed = version != self.selection;
        self.selection = version;
        changed
    }

    /// The last frame painted, for the reading, the view or a test.
    pub fn last_scene(&self) -> Option<&Scene> {
        self.last.as_ref()
    }

    /// Whether the last `show` moved the view, a line or the reading.
    pub fn changed(&self) -> bool {
        self.changed
    }

    /// The face a role is set in: egui's proportional font at the figure's
    /// size for it.
    pub fn font(&self, role: TextRole) -> FontId {
        FontId::proportional(self.figure.style(role.style_name()).unwrap_or(10.0) as f32)
    }

    fn sizes(&self) -> [f32; ROLES] {
        TextRole::ALL.map(|role| self.font(role).size)
    }

    /// Hand plotcore egui's fonts to measure with, again when a size changes.
    fn measure_with(&mut self, ctx: &egui::Context) {
        let sizes = self.sizes();
        if self.measured == Some(sizes) {
            return;
        }
        let (wide, tall) = (ctx.clone(), ctx.clone());
        let width =
            Box::new(move |text: &str, role: TextRole| {
                let font = FontId::proportional(sizes[role_slot(role)]);
                f64::from(wide.fonts_mut(|f| {
                    f.layout_no_wrap(text.to_owned(), font, Color32::WHITE).size().x
                }))
            });
        let height = Box::new(move |role: TextRole| {
            let font = FontId::proportional(sizes[role_slot(role)]);
            f64::from(tall.fonts_mut(|f| f.row_height(&font)))
        });
        if self.figure.set_text_measure(Some((width, height))).is_ok() {
            self.measured = Some(sizes);
        }
    }

    /// Lay the figure out in all the room `ui` has, hand it this frame's
    /// input, and paint it.
    pub fn show(&mut self, ui: &mut Ui) -> Response {
        let (rect, response) = ui.allocate_exact_size(ui.available_size(), Sense::click_and_drag());
        self.show_in(ui, rect, response)
    }

    fn show_in(&mut self, ui: &mut Ui, rect: Rect, response: Response) -> Response {
        self.changed = false;
        let ctx = ui.ctx().clone();
        self.measure_with(&ctx);
        let _ = self.figure.set_size(
            f64::from(rect.width()),
            f64::from(rect.height()),
            f64::from(ctx.pixels_per_point()),
        );

        // Cloned out of the input first: the figure measures text through the
        // context, and must not be called while the input holds it.
        let events = ui.input(|i| i.events.clone());
        let local = |p: Pos2| (f64::from(p.x - rect.min.x), f64::from(p.y - rect.min.y));
        for event in events {
            match event {
                Event::PointerMoved(pos) => {
                    if self.held.is_some() || rect.contains(pos) {
                        let (x, y) = local(pos);
                        let keys = modifiers(ui.input(|i| i.modifiers));
                        let _ = self.figure.move_to(x, y, keys);
                        self.inside = true;
                        self.changed |= self.held.is_some();
                    } else if self.inside {
                        let _ = self.figure.leave();
                        self.inside = false;
                    }
                }
                Event::PointerGone if self.inside && self.held.is_none() => {
                    let _ = self.figure.leave();
                    self.inside = false;
                }
                Event::PointerButton { pos, button: which, pressed, modifiers: keys } => {
                    let Some(which) = button(which) else { continue };
                    let (x, y) = local(pos);
                    if pressed && rect.contains(pos) && self.held.is_none() {
                        let _ = self.figure.press(x, y, which, modifiers(keys));
                        self.held = Some(which);
                    } else if !pressed && self.held == Some(which) {
                        let _ = self.figure.release(x, y, which, modifiers(keys));
                        self.held = None;
                        self.changed = true;
                    }
                }
                Event::MouseWheel { unit, delta, modifiers: keys, .. } => {
                    let Some(pos) = ui.input(|i| i.pointer.latest_pos()) else { continue };
                    if !rect.contains(pos) {
                        continue;
                    }
                    // Shift turns a vertical wheel horizontal on some platforms.
                    let notches = if delta.y != 0.0 { delta.y } else { delta.x };
                    let notches = match unit {
                        MouseWheelUnit::Point => notches / POINTS_PER_NOTCH,
                        MouseWheelUnit::Line => notches,
                        MouseWheelUnit::Page => notches * 3.0,
                    };
                    let (x, y) = local(pos);
                    let _ = self.figure.wheel(x, y, f64::from(notches) * 120.0, modifiers(keys));
                    self.changed = true;
                }
                _ => {}
            }
        }
        if response.double_clicked_by(PointerButton::Primary) {
            if let Some(pos) = response.interact_pointer_pos() {
                let (x, y) = local(pos);
                let _ = self.figure.double_click(x, y, Button::Left);
                self.changed = true;
            }
        }

        if let Ok(scene) = self.figure.scene() {
            let fonts = TextRole::ALL.map(|role| self.font(role));
            paint_scene(
                &ui.painter_at(rect),
                rect.min.to_vec2(),
                &scene,
                &fonts,
                self.strokes,
                &mut self.pictures,
            );
            self.last = Some(scene);
        }
        response
    }
}

fn pos(offset: Vec2, x: f64, y: f64) -> Pos2 {
    Pos2::new(x as f32, y as f32) + offset
}

fn draw_text(painter: &Painter, offset: Vec2, text: &Mark, fonts: &[FontId; ROLES]) {
    let ink = colour(text.colour);
    let base = if text.base.is_empty() { text.text.clone() } else { text.base.clone() };
    let base = painter.layout_no_wrap(base, fonts[role_slot(text.role)].clone(), ink);
    let exponent = (!text.exponent.is_empty()).then(|| {
        painter.layout_no_wrap(
            text.exponent.clone(),
            fonts[role_slot(TextRole::TickMinor)].clone(),
            ink,
        )
    });
    let width = base.size().x + exponent.as_ref().map_or(0.0, |e| e.size().x);
    let height = base.size().y;
    let shift = |align: Align, extent: f32| match align {
        Align::Start => 0.0,
        Align::Middle => -extent / 2.0,
        Align::End => -extent,
    };
    let (left, top) = (shift(text.across, width), shift(text.down, height));
    let at = pos(offset, text.x0, text.y0);
    // Turned clockwise about `at`, and aligned in the turned frame.
    let angle = (text.turn as f32).to_radians();
    let (sin, cos) = angle.sin_cos();
    let place = |dx: f32, dy: f32| at + Vec2::new(dx * cos - dy * sin, dx * sin + dy * cos);
    let raise = text.raise as f32;
    painter.add(TextShape::new(place(left, top), base.clone(), ink).with_angle(angle));
    if let Some(exponent) = exponent {
        painter.add(
            TextShape::new(place(left + base.size().x, top - raise), exponent, ink)
                .with_angle(angle),
        );
    }
}

fn paint_mark(
    painter: &Painter,
    pane: &Painter,
    offset: Vec2,
    mark: &Mark,
    fonts: &[FontId; ROLES],
) {
    let painter = if mark.clipped { pane } else { painter };
    let edge = Stroke::new(mark.width as f32, colour(mark.stroke));
    let fill = colour(mark.fill);
    match mark.kind {
        MarkKind::Segment => {
            painter
                .line_segment([pos(offset, mark.x0, mark.y0), pos(offset, mark.x1, mark.y1)], edge);
        }
        MarkKind::Box => {
            let r =
                Rect::from_two_pos(pos(offset, mark.x0, mark.y0), pos(offset, mark.x1, mark.y1));
            let corners = CornerRadius::same(mark.radius.round().clamp(0.0, 255.0) as u8);
            if mark.fill_to.alpha > 0.0 {
                // The colour bar's ramp: a quad from the fill at its top to
                // fill_to at its foot, which egui blends between its vertices.
                let (top, foot) = (fill, colour(mark.fill_to));
                let mut ramp = Mesh::default();
                ramp.colored_vertex(r.left_top(), top);
                ramp.colored_vertex(r.right_top(), top);
                ramp.colored_vertex(r.right_bottom(), foot);
                ramp.colored_vertex(r.left_bottom(), foot);
                ramp.add_triangle(0, 1, 2);
                ramp.add_triangle(0, 2, 3);
                painter.add(Shape::mesh(ramp));
            } else if mark.fill.alpha > 0.0 {
                painter.rect_filled(r, corners, fill);
            }
            // Filled to its bounds, and edged inside them.
            if mark.width > 0.0 {
                if mark.dashed {
                    let inner = r.shrink(mark.width as f32 / 2.0);
                    let path = [
                        inner.left_top(),
                        inner.right_top(),
                        inner.right_bottom(),
                        inner.left_bottom(),
                        inner.left_top(),
                    ];
                    let dash = 4.0 * mark.width as f32;
                    painter.extend(Shape::dashed_line(&path, edge, dash, dash));
                } else {
                    painter.rect_stroke(r, corners, edge, StrokeKind::Inside);
                }
            }
        }
        MarkKind::Circle => {
            let centre = pos(offset, mark.x0, mark.y0);
            if mark.fill.alpha > 0.0 {
                painter.circle_filled(centre, mark.radius as f32, fill);
            }
            if mark.width > 0.0 {
                painter.circle_stroke(centre, mark.radius as f32, edge);
            }
        }
        MarkKind::Text => draw_text(painter, offset, mark, fonts),
    }
}

/// Paint `scene` with `painter`, `offset` from its pane origin: all a host
/// other than [`PlotView`] needs to draw a figure in egui. Every colour, width
/// and place is the scene's; `pictures` holds the textures a scatter's image
/// is painted from, and lives as long as the host draws.
pub fn paint_scene(
    painter: &Painter,
    offset: Vec2,
    scene: &Scene,
    fonts: &[FontId; ROLES],
    strokes: Strokes,
    pictures: &mut Pictures,
) {
    let whole =
        Rect::from_min_size(offset.to_pos2(), Vec2::new(scene.width as f32, scene.height as f32));
    if scene.ground.alpha > 0.0 {
        painter.rect_filled(whole, CornerRadius::ZERO, colour(scene.ground));
    }
    let area = Rect::from_min_size(
        pos(offset, scene.area.x, scene.area.y),
        Vec2::new(scene.area.width as f32, scene.area.height as f32),
    );
    let pane = painter.with_clip_rect(area);
    for mark in &scene.under {
        paint_mark(painter, &pane, offset, mark, fonts);
    }

    let mut slot = 0;
    for line in &scene.lines {
        let ink = colour(line.colour);
        if let Some(image) = &line.image {
            // On the device pixels it was painted for: its corner is one, and
            // the offset is put on one first, as the bars' is.
            let scale = painter.pixels_per_point();
            let origin = (offset * scale).round() / scale;
            let at = Rect::from_min_size(
                Pos2::new(image.rect.x as f32, image.rect.y as f32) + origin,
                Vec2::new(image.rect.width as f32, image.rect.height as f32),
            );
            let texture = pictures.texture(painter.ctx(), slot, image);
            slot += 1;
            let whole = Rect::from_min_max(Pos2::ZERO, Pos2::new(1.0, 1.0));
            pane.image(texture, at, whole, Color32::WHITE);
        }
        if !line.bars.is_empty() {
            pane.add(Shape::mesh(bars_mesh(&line.bars, offset, ink, painter.pixels_per_point())));
        }
        match strokes {
            Strokes::Triangles => {
                let mut mesh = Mesh::default();
                for p in &line.triangles {
                    mesh.colored_vertex(pos(offset, p[0], p[1]), ink);
                }
                mesh.indices = (0..mesh.vertices.len() as u32).collect();
                pane.add(Shape::mesh(mesh));
            }
            Strokes::Feathered => {
                for run in &line.runs {
                    let points: Vec<Pos2> = run.iter().map(|p| pos(offset, p[0], p[1])).collect();
                    pane.add(Shape::line(points, Stroke::new(line.width as f32, ink)));
                }
            }
        }
        // A scatter's markers each in a colour of their own, when they have
        // one, in the order given: the later over the earlier.
        let own = line.marker_colours.len() == line.markers.len();
        for (k, m) in line.markers.iter().enumerate() {
            let fill = if own { colour(line.marker_colours[k]) } else { ink };
            pane.circle_filled(pos(offset, m[0], m[1]), line.marker_radius as f32, fill);
        }
    }

    for mark in &scene.over {
        paint_mark(painter, &pane, offset, mark, fonts);
    }
}

/// A run of device pixels from `from` to `to`, as the whole-pixel stretch of it
/// and the part-covered pixel at each end: (start, end, how much is covered).
fn device_spans(from: f32, to: f32) -> impl Iterator<Item = (f32, f32, f32)> {
    // Within this of a whole pixel is that pixel: a coordinate that has been
    // through a scale in f32 is seldom exactly on one, and a sliver of a pixel
    // would be painted as a quad of its own.
    const NEAR: f32 = 1.0 / 256.0;
    let snap = |v: f32| if (v - v.round()).abs() < NEAR { v.round() } else { v };
    let (from, to) = (snap(from), snap(to));
    let (whole_from, whole_to) = (from.ceil(), to.floor());
    let spans = if whole_from > whole_to {
        // Inside one pixel, touching neither of its edges.
        [(from.floor(), from.floor() + 1.0, to - from), (0.0, 0.0, 0.0), (0.0, 0.0, 0.0)]
    } else {
        [
            (whole_from - 1.0, whole_from, whole_from - from),
            (whole_from, whole_to, 1.0),
            (whole_to, whole_to + 1.0, to - whole_to),
        ]
    };
    spans.into_iter().filter(|&(start, end, cover)| end > start && cover > 0.0)
}

/// A line's bars as a mesh on device pixels.
///
/// A mesh is not antialiased, so what QPainter works out for itself is written
/// down here: the pixel each end of a bar part covers is a quad of its own, at
/// that part of the strength. Without it the ends round to a whole pixel, and a
/// gently sloping stretch of line, every bar of which is about a pixel tall, is
/// a staircase. The offset is put on a device pixel first, because a widget may
/// sit between two, and every bar would then be between two as well.
fn bars_mesh(bars: &[[f64; 2]], offset: Vec2, ink: Color32, pixels_per_point: f32) -> Mesh {
    let scale = pixels_per_point;
    let origin = (offset * scale).round();
    let mut mesh = Mesh::default();
    for corners in bars.chunks_exact(2) {
        let across = device_spans(
            origin.x + corners[0][0] as f32 * scale,
            origin.x + corners[1][0] as f32 * scale,
        );
        for (x0, x1, cover_x) in across {
            let along = device_spans(
                origin.y + corners[0][1] as f32 * scale,
                origin.y + corners[1][1] as f32 * scale,
            );
            for (y0, y1, cover_y) in along {
                mesh.add_colored_rect(
                    Rect::from_min_max(
                        Pos2::new(x0 / scale, y0 / scale),
                        Pos2::new(x1 / scale, y1 / scale),
                    ),
                    ink.gamma_multiply(cover_x * cover_y),
                );
            }
        }
    }
    mesh
}

#[cfg(test)]
mod tests {
    use super::*;
    use egui::{RawInput, TouchPhase};

    fn input(events: Vec<Event>) -> RawInput {
        RawInput {
            screen_rect: Some(Rect::from_min_size(Pos2::ZERO, Vec2::new(800.0, 500.0))),
            events,
            ..RawInput::default()
        }
    }

    fn view_of(lines: &[Vec<f64>]) -> PlotView {
        let mut figure = Figure::new();
        for (i, values) in lines.iter().enumerate() {
            figure.add_line(values, &format!("line {i}")).unwrap();
        }
        PlotView::new(figure)
    }

    /// One headless frame, and the shapes it painted. Its texture deltas have
    /// no renderer to go to, which egui insists is said rather than implied.
    fn frame(
        ctx: &egui::Context,
        view: &mut PlotView,
        events: Vec<Event>,
    ) -> Vec<egui::epaint::ClippedShape> {
        let output = ctx.run_ui(input(events), |ui| {
            view.show(ui);
        });
        let shapes = output.shapes.clone();
        output.drop_without_applying_deltas();
        shapes
    }

    #[test]
    fn a_frame_paints_each_line_as_its_triangles() {
        let ctx = egui::Context::default();
        let mut view = view_of(&[(0..10_000).map(|i| (i as f64 / 100.0).sin()).collect()]);
        let shapes = frame(&ctx, &mut view, vec![]);
        let scene = view.last_scene().expect("a frame was painted");
        assert_eq!(scene.lines.len(), 1);
        let meshes = shapes
            .iter()
            .filter(|clipped| matches!(&clipped.shape, Shape::Mesh(mesh) if mesh.vertices.len() == scene.lines[0].triangles.len()))
            .count();
        assert_eq!(meshes, 1, "the line is plotcore's own triangles");
        // egui measured the numbers: the left gutter is as wide as they are.
        assert!(scene.area.x > 0.0 && scene.texts().next().is_some());
    }

    #[test]
    fn a_dense_line_is_painted_as_bars_on_whole_pixels() {
        let ctx = egui::Context::default();
        let mut view = view_of(&[(0..200_000).map(|i| ((i % 977) as f64).sqrt()).collect()]);
        view.figure.set_text("drawing", "bars").unwrap();
        let shapes = frame(&ctx, &mut view, vec![]);
        let scene = view.last_scene().expect("a frame was painted");
        assert!(!scene.lines[0].bars.is_empty());
        assert!(scene.lines[0].triangles.is_empty(), "bars in place of the stroke");
        let mesh = shapes
            .iter()
            .find_map(|clipped| match &clipped.shape {
                Shape::Mesh(mesh) if !mesh.vertices.is_empty() => Some(mesh),
                _ => None,
            })
            .expect("the bars are a mesh");
        for vertex in &mesh.vertices {
            assert_eq!(vertex.pos.x, vertex.pos.x.round(), "a bar is on whole pixels across");
            assert_eq!(vertex.pos.y, vertex.pos.y.round(), "and each part of it along");
        }
    }

    #[test]
    fn a_line_with_both_forms_is_painted_as_both() {
        let ctx = egui::Context::default();
        let values: Vec<f64> = (0..400_000)
            .map(|i| {
                if (150_000..250_000).contains(&i) {
                    if i % 2 == 0 {
                        -1.0
                    } else {
                        1.0
                    }
                } else {
                    (f64::from(i) * 2e-5).sin()
                }
            })
            .collect();
        let mut view = view_of(&[values]);
        view.figure.set_text("drawing", "adaptive").unwrap();
        let shapes = frame(&ctx, &mut view, vec![]);
        let scene = view.last_scene().expect("a frame was painted");
        let line = &scene.lines[0];
        assert!(!line.bars.is_empty() && !line.triangles.is_empty());
        let meshes = shapes
            .iter()
            .filter(|clipped| matches!(&clipped.shape, Shape::Mesh(m) if !m.vertices.is_empty()))
            .count();
        assert_eq!(meshes, 2, "the bars, and the stroke beside them");
    }

    #[test]
    fn a_bar_s_ends_are_the_part_of_a_pixel_they_cover() {
        let spans: Vec<_> = device_spans(10.25, 13.5).collect();
        assert_eq!(spans, vec![(10.0, 11.0, 0.75), (11.0, 13.0, 1.0), (13.0, 14.0, 0.5)]);
        let inside: Vec<_> = device_spans(10.25, 10.75).collect();
        assert_eq!(inside, vec![(10.0, 11.0, 0.5)]);
        let whole: Vec<_> = device_spans(4.0, 5.0).collect();
        assert_eq!(whole, vec![(4.0, 5.0, 1.0)]);
        let nearly: Vec<_> = device_spans(4.001, 4.999).collect();
        assert_eq!(nearly, vec![(4.0, 5.0, 1.0)], "not a sliver of a pixel at each side");
    }

    #[test]
    fn a_dense_scatter_is_painted_as_one_texture_and_a_sparse_one_as_dots() {
        let n = 200_000;
        let mut state = 7u32;
        let mut draw = || {
            state = state.wrapping_mul(1_103_515_245).wrapping_add(12_345);
            f64::from((state >> 16) & 0x7fff) / 32767.0
        };
        let x: Vec<f64> = (0..n).map(|_| draw()).collect();
        let y: Vec<f64> = (0..n).map(|_| draw()).collect();
        let mut figure = Figure::new();
        figure.add_points(&x, &y, "cloud").unwrap();
        figure.add_points(&x[..200], &y[..200], "few").unwrap();
        let mut view = PlotView::new(figure);
        let ctx = egui::Context::default();
        let output = frame(&ctx, &mut view, vec![]);
        let scene = view.last_scene().unwrap();
        assert!(scene.lines[0].image.is_some());
        assert_eq!(scene.lines[1].markers.len(), 200);
        let textured = output
            .iter()
            .filter(|clipped| {
                matches!(&clipped.shape, Shape::Mesh(mesh) if mesh.texture_id != TextureId::default())
            })
            .count();
        assert_eq!(textured, 1, "the dense one, as one texture");
        let dots =
            output.iter().filter(|clipped| matches!(&clipped.shape, Shape::Circle(_))).count();
        assert!(dots >= 200, "the sparse one, a dot each");
        // The texture is the one it had, next frame.
        let again = frame(&ctx, &mut view, vec![]);
        let ids: Vec<TextureId> = [&output, &again]
            .iter()
            .flat_map(|shapes| shapes.iter())
            .filter_map(|clipped| match &clipped.shape {
                Shape::Mesh(mesh) if mesh.texture_id != TextureId::default() => {
                    Some(mesh.texture_id)
                }
                _ => None,
            })
            .collect();
        assert_eq!(ids.len(), 2);
        assert_eq!(ids[0], ids[1]);
    }

    #[test]
    fn egui_s_wheel_and_drag_are_the_figure_s_gestures() {
        let ctx = egui::Context::default();
        let mut view = view_of(&[(0..10_000).map(f64::from).collect()]);
        frame(&ctx, &mut view, vec![]);
        let before = view.figure.view();
        let middle = Pos2::new(400.0, 250.0);
        frame(&ctx, &mut view, vec![Event::PointerMoved(middle)]);
        frame(
            &ctx,
            &mut view,
            vec![Event::MouseWheel {
                unit: MouseWheelUnit::Line,
                delta: Vec2::new(0.0, 2.0),
                phase: TouchPhase::Move,
                source: egui::MouseWheelSource::Unknown,
                modifiers: egui::Modifiers::NONE,
            }],
        );
        let zoomed = view.figure.view();
        assert!(zoomed[1] - zoomed[0] < before[1] - before[0]);
        assert!(view.changed());

        let press = |pressed, at| Event::PointerButton {
            pos: at,
            button: PointerButton::Primary,
            pressed,
            modifiers: egui::Modifiers::NONE,
        };
        let moved = middle + Vec2::new(60.0, 0.0);
        frame(&ctx, &mut view, vec![press(true, middle)]);
        frame(&ctx, &mut view, vec![Event::PointerMoved(moved)]);
        frame(&ctx, &mut view, vec![press(false, moved)]);
        assert!(view.figure.view()[0] < zoomed[0], "a drag to the right shows what is to the left");
    }

    #[test]
    fn the_reading_follows_the_pointer_and_leaves_with_it() {
        let ctx = egui::Context::default();
        let values: Vec<f64> = (0..300).map(|i| 10.0 + 0.5 * f64::from(i)).collect();
        let mut view = view_of(std::slice::from_ref(&values));
        frame(&ctx, &mut view, vec![]);
        let [x, y] = view.last_scene().unwrap().lines[0].runs[0][120];
        frame(&ctx, &mut view, vec![Event::PointerMoved(Pos2::new(x as f32, y as f32))]);
        let reading = view.last_scene().unwrap().reading.clone().expect("a reading");
        assert_eq!((reading.x, reading.y), (120.0, values[120]));
        frame(&ctx, &mut view, vec![Event::PointerGone]);
        assert!(view.last_scene().unwrap().reading.is_none());
    }
}
