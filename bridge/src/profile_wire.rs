use crate::core_ffi::ProfileView;

pub fn encode_profile(view: &ProfileView) -> Result<Vec<u8>, String> {
    let descriptor: serde_json::Value =
        serde_json::from_str(view.descriptor_json()).map_err(|e| e.to_string())?;
    let metadata = serde_json::to_vec(
        &serde_json::json!({"descriptor":descriptor,"cloudId":view.cloud_id().to_string()}),
    )
    .map_err(|e| e.to_string())?;
    if metadata.len() > 16384 {
        return Err("Profile metadata exceeds 8KiB".into());
    }
    let mut out = Vec::new();
    for value in [
        0x5250594cu32,
        1,
        view.point_count(),
        view.total_points(),
        view.offset(),
        view.flags(),
        metadata.len() as u32,
        0,
    ] {
        out.extend_from_slice(&value.to_le_bytes());
    }
    out.extend_from_slice(&metadata);
    pad(&mut out);
    for channel in [view.xyz(), view.intensity(), view.normals()] {
        for value in channel {
            out.extend_from_slice(&value.to_le_bytes());
        }
    }
    out.extend_from_slice(view.rgb());
    pad(&mut out);
    out.extend_from_slice(view.validity());
    pad(&mut out);
    for value in view.source_indices() {
        out.extend_from_slice(&value.to_le_bytes());
    }
    Ok(out)
}

fn pad(out: &mut Vec<u8>) {
    out.resize((out.len() + 3) & !3, 0);
}
