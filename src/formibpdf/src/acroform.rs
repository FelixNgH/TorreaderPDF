// acroform.rs — AcroForm field extraction (read-only, v2.0)

use crate::pdf_object::PdfObject;
use crate::xref::XRefTable;
use std::collections::HashMap;

#[derive(Debug, Clone)]
pub struct AcroField {
    /// Partial field name from /T
    pub name: String,
    /// Current value from /V
    pub value: String,
    /// 0=text(Tx), 1=button(Btn), 2=choice(Ch), 3=signature(Sig)
    pub field_type: u32,
    /// Page rectangle [x0,y0,x1,y1] in PDF points
    pub rect: Option<(f64, f64, f64, f64)>,
}

/// Extract all terminal AcroForm fields from the document catalog.
pub fn extract_fields(
    catalog: &HashMap<String, PdfObject>,
    xref: &XRefTable,
    data: &[u8],
) -> Vec<AcroField> {
    let acroform_ref = match catalog.get("AcroForm") {
        Some(o) => o,
        None    => return Vec::new(),
    };
    let acroform = xref.resolve(data, acroform_ref);
    let fields_arr = match acroform.dict_get("Fields")
        .and_then(|o| if let PdfObject::Array(_) = xref.resolve(data, o) { Some(xref.resolve(data, o)) } else { None })
    {
        Some(PdfObject::Array(a)) => a,
        _ => return Vec::new(),
    };

    let mut out = Vec::new();
    for field_ref in &fields_arr {
        collect_field(field_ref, None, xref, data, &mut out);
    }
    out
}

fn collect_field(
    obj:        &PdfObject,
    parent_ft:  Option<u32>,
    xref:       &XRefTable,
    data:       &[u8],
    out:        &mut Vec<AcroField>,
) {
    let resolved = xref.resolve(data, obj);
    let dict = match &resolved {
        PdfObject::Dict(d)             => d.clone(),
        PdfObject::Stream { dict, .. } => dict.clone(),
        _ => return,
    };

    // Field type: /FT (may be inherited from parent)
    let ft_str = dict.get("FT").and_then(|o| o.as_name()).map(|s| s.to_string());
    let ft: u32 = ft_str.as_deref()
        .map(|s| match s { "Btn" => 1, "Ch" => 2, "Sig" => 3, _ => 0 })
        .or(parent_ft)
        .unwrap_or(0);

    // If this field has /Kids, recurse
    if let Some(PdfObject::Array(kids)) = dict.get("Kids") {
        let kids_clone = kids.clone();
        for kid in &kids_clone {
            collect_field(kid, Some(ft), xref, data, out);
        }
        return;
    }

    // Terminal field
    let name = dict.get("T")
        .and_then(|o| match o {
            PdfObject::Str(v) => String::from_utf8(v.clone()).ok(),
            PdfObject::Name(n) => Some(n.clone()),
            _ => None,
        })
        .unwrap_or_default();

    let value = dict.get("V")
        .map(|v| obj_to_string(v))
        .unwrap_or_default();

    let rect = dict.get("Rect")
        .and_then(|o| o.as_array())
        .and_then(|a| {
            if a.len() >= 4 {
                Some((
                    a[0].as_f64()?,
                    a[1].as_f64()?,
                    a[2].as_f64()?,
                    a[3].as_f64()?,
                ))
            } else { None }
        });

    out.push(AcroField { name, value, field_type: ft, rect });
}

fn obj_to_string(obj: &PdfObject) -> String {
    match obj {
        PdfObject::Str(v)     => String::from_utf8_lossy(v).into_owned(),
        PdfObject::Name(n)    => n.clone(),
        PdfObject::Integer(i) => i.to_string(),
        PdfObject::Real(r)    => format!("{:.4}", r),
        PdfObject::Bool(b)    => b.to_string(),
        PdfObject::Array(a)   => a.iter().map(obj_to_string).collect::<Vec<_>>().join(", "),
        _ => String::new(),
    }
}
