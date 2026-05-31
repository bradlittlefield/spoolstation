import { useState, useEffect, useCallback } from "react";

// CONFIG
const SPOOLMAN_BASE = "http://raspberrypi.local:7912";  // change to your RPi IP
const BRIDGE_BASE   = "http://raspberrypi.local:8765";  // spooldesk_bridge.py

// COLOR LOOKUP via filamentcolors.xyz (proxied through bridge to avoid CORS)
async function lookupFilamentColor(vendor, colorName, material = "PLA") {
  if (!vendor || !colorName) return null;
  try {
    const params = new URLSearchParams({ vendor, color_name: colorName, material });
    const r = await fetch(`${BRIDGE_BASE}/colors/lookup?${params}`);
    if (!r.ok) return null;
    const data = await r.json();
    if (data.error) return null;
    return data;
  } catch { return null; }
}

async function fetchVendorColors(vendor, material = "") {
  if (!vendor) return [];
  try {
    const params = new URLSearchParams({ vendor, ...(material ? { material } : {}) });
    const r = await fetch(`${BRIDGE_BASE}/colors/vendor?${params}`);
    if (!r.ok) return [];
    const data = await r.json();
    return data.colors || [];
  } catch { return []; }
}

const MATERIAL_DENSITIES = {
  PLA: 1.24, "PLA+": 1.22, "PLA-CF": 1.30,
  PETG: 1.27, "PETG-CF": 1.35,
  ABS: 1.04, ASA: 1.07,
  TPU: 1.21, TPE: 1.20,
  Nylon: 1.08, "Nylon-CF": 1.15,
  HIPS: 1.07, PC: 1.20,
  "PVA": 1.23, BVOH: 1.23,
};

const SPOOL_TARE_WEIGHTS = {
  Prusament: 201, eSun: 220, Polymaker: 200,
  Bambu: 165, Sunlu: 130, Hatchbox: 230,
  OVERTURE: 210, PolyTerra: 140, Generic: 215,
};

function calcRemainingLength(remainingGrams, material = "PLA", diameterMm = 1.75) {
  const density = MATERIAL_DENSITIES[material] || 1.24;
  const r = diameterMm / 2 / 10; // cm
  const volume = remainingGrams / density; // cm³
  const length = volume / (Math.PI * r * r); // cm
  return Math.round(length / 100); // meters
}

const MOCK_SPOOLS = [
  { id: 1, tag_uid: "04:AB:12:CD:EF:01", vendor: "Prusament", material: "PLA", name: "Galaxy Purple", color_hex: "#6B3FA0", diameter: 1.75, density: 1.24, tare_weight: 201, gross_weight: 843, remaining_weight: 642, toolhead: 1 },
  { id: 2, tag_uid: "04:CD:34:AB:78:02", vendor: "Bambu", material: "PETG", name: "Jade White", color_hex: "#E8EEF0", diameter: 1.75, density: 1.27, tare_weight: 165, gross_weight: 621, remaining_weight: 456, toolhead: 2 },
  { id: 3, tag_uid: "04:EF:56:12:34:03", vendor: "eSun", material: "ABS", name: "Fire Red", color_hex: "#D42B2B", diameter: 1.75, density: 1.04, tare_weight: 220, gross_weight: 1003, remaining_weight: 783, toolhead: null },
  { id: 4, tag_uid: "04:12:78:CD:AB:04", vendor: "Sunlu", material: "TPU", name: "Midnight Black", color_hex: "#1A1A1A", diameter: 1.75, density: 1.21, tare_weight: 130, gross_weight: 550, remaining_weight: 420, toolhead: null },
  { id: 5, tag_uid: "04:34:9A:EF:56:05", vendor: "Polymaker", material: "PLA+", name: "Matte Cream", color_hex: "#F5ECD7", diameter: 1.75, density: 1.22, tare_weight: 200, gross_weight: 412, remaining_weight: 212, toolhead: 3 },
  { id: 6, tag_uid: "04:56:BC:12:78:06", vendor: "Prusament", material: "PETG", name: "Prusa Orange", color_hex: "#FA6831", diameter: 1.75, density: 1.27, tare_weight: 201, gross_weight: 987, remaining_weight: 786, toolhead: null },
];

const MOCK_LOG = [
  { id: 1, ts: "2026-05-27T14:32:11", spool_id: 1, vendor: "Prusament", name: "Galaxy Purple", old_weight: 680, new_weight: 642, delta: -38, action: "weighed" },
  { id: 2, ts: "2026-05-27T11:10:04", spool_id: 5, vendor: "Polymaker", name: "Matte Cream", old_weight: 250, new_weight: 212, delta: -38, action: "weighed" },
  { id: 3, ts: "2026-05-26T19:55:30", spool_id: 2, vendor: "Bambu", name: "Jade White", old_weight: 501, new_weight: 456, delta: -45, action: "weighed" },
  { id: 4, ts: "2026-05-26T08:22:18", spool_id: 3, vendor: "eSun", name: "Fire Red", old_weight: 820, new_weight: 783, delta: -37, action: "weighed" },
  { id: 5, ts: "2026-05-25T16:40:02", spool_id: 6, vendor: "Prusament", name: "Prusa Orange", old_weight: 1000, new_weight: 786, delta: -214, action: "weighed" },
];

const TOOLHEAD_COLORS = { 1: "#00C9FF", 2: "#7FFF00", 3: "#FF9500", 4: "#FF3B6B" };

// --- ICONS -------------------------------------------------------------------
const IconSpool = () => (
  <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round">
    <circle cx="12" cy="12" r="10"/><circle cx="12" cy="12" r="3"/>
    <line x1="12" y1="2" x2="12" y2="9"/><line x1="12" y1="15" x2="12" y2="22"/>
    <line x1="2" y1="12" x2="9" y2="12"/><line x1="15" y1="12" x2="22" y2="12"/>
  </svg>
);
const IconScale = () => (
  <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round">
    <path d="M12 3v1m0 16v1M3 12h1m16 0h1"/><path d="M5.6 5.6l.7.7m11.4 11.4.7.7M5.6 18.4l.7-.7M18.4 5.6l-.7.7"/>
    <circle cx="12" cy="12" r="4"/><path d="M4 20h16"/>
  </svg>
);
const IconRFID = () => (
  <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round">
    <path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M1.42 9a16 16 0 0 1 21.16 0"/>
    <path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><circle cx="12" cy="20" r="1"/>
  </svg>
);
const IconPrinter = () => (
  <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round">
    <polyline points="6 9 6 2 18 2 18 9"/><path d="M6 18H4a2 2 0 0 1-2-2v-5a2 2 0 0 1 2-2h16a2 2 0 0 1 2 2v5a2 2 0 0 1-2 2h-2"/>
    <rect x="6" y="14" width="12" height="8"/><line x1="9" y1="7" x2="15" y2="7"/>
  </svg>
);
const IconActivity = () => (
  <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round">
    <polyline points="22 12 18 12 15 21 9 3 6 12 2 12"/>
  </svg>
);
const IconPlus = () => (
  <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.2" strokeLinecap="round">
    <line x1="12" y1="5" x2="12" y2="19"/><line x1="5" y1="12" x2="19" y2="12"/>
  </svg>
);
const IconSearch = () => (
  <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round">
    <circle cx="11" cy="11" r="8"/><line x1="21" y1="21" x2="16.65" y2="16.65"/>
  </svg>
);
const IconX = () => (
  <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round">
    <line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/>
  </svg>
);
const IconChevron = ({ dir = "down" }) => (
  <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round"
    style={{ transform: dir === "up" ? "rotate(180deg)" : "none" }}>
    <polyline points="6 9 12 15 18 9"/>
  </svg>
);
const IconSync = () => (
  <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round">
    <path d="M23 4v6h-6"/><path d="M1 20v-6h6"/>
    <path d="M3.51 9a9 9 0 0 1 14.85-3.36L23 10M1 14l4.64 4.36A9 9 0 0 0 20.49 15"/>
  </svg>
);

// --- PERCENTAGE BAR -----------------------------------------------------------
function FilamentBar({ remaining, total = 1000, color = "#00C9FF" }) {
  const pct = Math.max(0, Math.min(100, Math.round((remaining / total) * 100)));
  const barColor = pct < 15 ? "#FF3B6B" : pct < 30 ? "#FF9500" : color;
  return (
    <div style={{ display: "flex", alignItems: "center", gap: "10px" }}>
      <div style={{
        flex: 1, height: "6px", background: "rgba(255,255,255,0.07)",
        borderRadius: "3px", overflow: "hidden"
      }}>
        <div style={{
          width: `${pct}%`, height: "100%", background: barColor,
          borderRadius: "3px", transition: "width 0.8s cubic-bezier(0.4,0,0.2,1)",
          boxShadow: `0 0 8px ${barColor}60`
        }} />
      </div>
      <span style={{ fontSize: "11px", color: barColor, fontFamily: "monospace", minWidth: "36px", textAlign: "right" }}>{pct}%</span>
    </div>
  );
}

// --- SPOOL CARD ---------------------------------------------------------------
function SpoolCard({ spool, onEdit }) {
  const remaining = spool.remaining_weight;
  const totalFilament = spool.gross_weight - spool.tare_weight;
  const lengthM = calcRemainingLength(remaining, spool.material, spool.diameter);
  const isLow = remaining < 100;
  const isMedium = remaining >= 100 && remaining < 250;

  return (
    <div style={{
      background: "rgba(255,255,255,0.033)",
      border: `1px solid ${isLow ? "#FF3B6B44" : "rgba(255,255,255,0.07)"}`,
      borderRadius: "10px", padding: "16px",
      transition: "all 0.2s ease",
      cursor: "pointer",
      position: "relative", overflow: "hidden"
    }}
      onClick={() => onEdit(spool)}
      onMouseEnter={e => { e.currentTarget.style.background = "rgba(255,255,255,0.06)"; e.currentTarget.style.borderColor = "rgba(255,255,255,0.15)"; }}
      onMouseLeave={e => { e.currentTarget.style.background = "rgba(255,255,255,0.033)"; e.currentTarget.style.borderColor = isLow ? "#FF3B6B44" : "rgba(255,255,255,0.07)"; }}
    >
      {/* Color accent top bar */}
      <div style={{ position: "absolute", top: 0, left: 0, right: 0, height: "2px", background: spool.color_hex, opacity: 0.8 }} />

      <div style={{ display: "flex", justifyContent: "space-between", alignItems: "flex-start", marginBottom: "12px" }}>
        <div style={{ display: "flex", alignItems: "center", gap: "10px" }}>
          {/* Color dot */}
          <div style={{
            width: "28px", height: "28px", borderRadius: "50%",
            background: spool.color_hex, flexShrink: 0,
            border: "2px solid rgba(255,255,255,0.15)",
            boxShadow: `0 0 12px ${spool.color_hex}60`
          }} />
          <div>
            <div style={{ fontSize: "13px", fontWeight: "600", color: "#F0F0F0", letterSpacing: "0.01em" }}>{spool.name}</div>
            <div style={{ fontSize: "11px", color: "rgba(255,255,255,0.45)", marginTop: "1px" }}>{spool.vendor} · {spool.material}</div>
          </div>
        </div>
        <div style={{ display: "flex", flexDirection: "column", alignItems: "flex-end", gap: "4px" }}>
          {spool.toolhead && (
            <div style={{
              fontSize: "10px", fontFamily: "monospace", fontWeight: "700",
              background: TOOLHEAD_COLORS[spool.toolhead] + "22",
              color: TOOLHEAD_COLORS[spool.toolhead],
              border: `1px solid ${TOOLHEAD_COLORS[spool.toolhead]}55`,
              padding: "2px 8px", borderRadius: "4px"
            }}>T{spool.toolhead}</div>
          )}
          {isLow && <div style={{ fontSize: "10px", color: "#FF3B6B", fontWeight: "700", letterSpacing: "0.05em" }}>LOW</div>}
        </div>
      </div>

      <FilamentBar remaining={remaining} total={totalFilament} color={spool.color_hex} />

      <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr 1fr", gap: "8px", marginTop: "12px" }}>
        {[
          { label: "REMAINING", value: `${remaining}g` },
          { label: "LENGTH", value: `~${lengthM}m` },
          { label: "DIAMETER", value: `${spool.diameter}mm` },
        ].map(({ label, value }) => (
          <div key={label} style={{ background: "rgba(255,255,255,0.04)", borderRadius: "6px", padding: "7px 8px" }}>
            <div style={{ fontSize: "9px", color: "rgba(255,255,255,0.35)", letterSpacing: "0.08em", marginBottom: "3px" }}>{label}</div>
            <div style={{ fontSize: "13px", fontFamily: "monospace", color: "#E0E0E0", fontWeight: "600" }}>{value}</div>
          </div>
        ))}
      </div>

      <div style={{ marginTop: "10px", fontSize: "10px", color: "rgba(255,255,255,0.25)", fontFamily: "monospace" }}>
        UID: {spool.tag_uid}
      </div>
    </div>
  );
}

// --- ADD / EDIT SPOOL MODAL ---------------------------------------------------
function SpoolModal({ spool, onClose, onSave }) {
  const isNew = !spool;
  const [form, setForm] = useState(spool || {
    vendor: "Generic", material: "PLA", name: "", color_hex: "#FF6B35",
    diameter: 1.75, tare_weight: 215, gross_weight: 1000, remaining_weight: 1000,
    tag_uid: "", toolhead: null
  });

  // Color lookup state
  const [colorLookupState, setColorLookupState] = useState("idle"); // idle | loading | found | not_found | error
  const [colorMatch, setColorMatch] = useState(null);               // {hex_color, color_name, swatch_url, image_card, td}
  const [vendorColors, setVendorColors] = useState([]);             // all colors for current vendor
  const [showVendorPicker, setShowVendorPicker] = useState(false);

  const density = MATERIAL_DENSITIES[form.material] || 1.24;
  const remaining = form.remaining_weight || 0;
  const lengthM = calcRemainingLength(remaining, form.material, form.diameter);

  const set = (k, v) => setForm(f => ({ ...f, [k]: v }));

  // Auto-lookup when both vendor + name are filled and name loses focus
  const handleColorLookup = async () => {
    if (!form.vendor || !form.name) return;
    setColorLookupState("loading");
    setColorMatch(null);
    const result = await lookupFilamentColor(form.vendor, form.name, form.material);
    if (result) {
      setColorLookupState("found");
      setColorMatch(result);
    } else {
      setColorLookupState("not_found");
    }
  };

  const applyColorMatch = (hex) => {
    set("color_hex", hex.startsWith("#") ? hex : `#${hex}`);
    setShowVendorPicker(false);
  };

  const handleLoadVendorColors = async () => {
    if (vendorColors.length > 0) { setShowVendorPicker(v => !v); return; }
    setShowVendorPicker(true);
    const colors = await fetchVendorColors(form.vendor, form.material);
    setVendorColors(colors);
  };

  // Reset color state when vendor changes
  const handleVendorChange = (v) => {
    set("vendor", v);
    setColorLookupState("idle");
    setColorMatch(null);
    setVendorColors([]);
    setShowVendorPicker(false);
  };

  const fieldStyle = {
    width: "100%", background: "rgba(255,255,255,0.06)", border: "1px solid rgba(255,255,255,0.1)",
    borderRadius: "6px", color: "#F0F0F0", padding: "9px 12px", fontSize: "13px",
    fontFamily: "monospace", outline: "none", boxSizing: "border-box"
  };
  const labelStyle = { fontSize: "10px", color: "rgba(255,255,255,0.4)", letterSpacing: "0.08em", marginBottom: "5px", display: "block" };

  return (
    <div style={{
      position: "fixed", inset: 0, background: "rgba(0,0,0,0.75)",
      display: "flex", alignItems: "center", justifyContent: "center",
      zIndex: 1000, padding: "20px", backdropFilter: "blur(4px)"
    }} onClick={e => e.target === e.currentTarget && onClose()}>
      <div style={{
        background: "#141414", border: "1px solid rgba(255,255,255,0.1)",
        borderRadius: "14px", padding: "28px", width: "100%", maxWidth: "520px",
        maxHeight: "90vh", overflowY: "auto"
      }}>
        <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: "24px" }}>
          <div style={{ fontSize: "16px", fontWeight: "700", color: "#F0F0F0" }}>
            {isNew ? "Register New Spool" : "Edit Spool"}
          </div>
          <button onClick={onClose} style={{ background: "none", border: "none", color: "rgba(255,255,255,0.4)", cursor: "pointer", padding: "4px" }}>
            <IconX />
          </button>
        </div>

        <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: "14px" }}>
          <div style={{ gridColumn: "1/-1" }}>
            <label style={labelStyle}>SPOOL NAME</label>
            <input style={fieldStyle} value={form.name} onChange={e => set("name", e.target.value)} onBlur={handleColorLookup} placeholder="e.g. Galaxy Purple - color auto-lookups on blur" />
          </div>
          <div>
            <label style={labelStyle}>VENDOR</label>
            <select style={fieldStyle} value={form.vendor} onChange={e => handleVendorChange(e.target.value)}>
              {["Prusament","Bambu Lab","Bambu","Kingroon","eSun","Polymaker","Sunlu","Hatchbox","OVERTURE","PolyTerra","Atomic Filament","Matterhackers","Proto-pasta","Generic"].map(v => <option key={v}>{v}</option>)}
            </select>
          </div>
          <div>
            <label style={labelStyle}>MATERIAL</label>
            <select style={fieldStyle} value={form.material} onChange={e => set("material", e.target.value)}>
              {Object.keys(MATERIAL_DENSITIES).map(m => <option key={m}>{m}</option>)}
            </select>
          </div>

          {/* COLOR with filamentcolors.xyz lookup */}
          <div style={{ gridColumn: "1/-1" }}>
            <label style={labelStyle}>
              COLOR
              {colorMatch && (
                <a href={colorMatch.swatch_url} target="_blank" rel="noreferrer"
                  style={{ marginLeft: "8px", color: "#00C9FF", fontSize: "9px", textDecoration: "none" }}>
                  via filamentcolors.xyz >
                </a>
              )}
            </label>

            {/* Main color row */}
            <div style={{ display: "flex", gap: "8px", alignItems: "center" }}>
              <input type="color" value={form.color_hex} onChange={e => set("color_hex", e.target.value)}
                style={{ width: "42px", height: "37px", border: "1px solid rgba(255,255,255,0.1)", borderRadius: "6px", cursor: "pointer", background: "none", padding: "2px", flexShrink: 0 }} />
              <input style={{ ...fieldStyle, flex: 1 }} value={form.color_hex} onChange={e => set("color_hex", e.target.value)} placeholder="#RRGGBB" />
              <button onClick={handleColorLookup} disabled={colorLookupState === "loading"} title="Look up measured hex from filamentcolors.xyz"
                style={{
                  padding: "9px 12px", borderRadius: "6px", fontSize: "11px", fontWeight: "600", whiteSpace: "nowrap",
                  background: colorLookupState === "found" ? "#7FFF0018" : "#00C9FF18",
                  border: `1px solid ${colorLookupState === "found" ? "#7FFF0055" : "#00C9FF44"}`,
                  color: colorLookupState === "found" ? "#7FFF00" : "#00C9FF",
                  cursor: colorLookupState === "loading" ? "wait" : "pointer", flexShrink: 0
                }}>
                {colorLookupState === "loading" ? "..." : colorLookupState === "found" ? "v MATCHED" : "LOOKUP"}
              </button>
              <button onClick={handleLoadVendorColors} title="Browse all measured colors for this vendor"
                style={{
                  padding: "9px 10px", borderRadius: "6px", fontSize: "11px", fontWeight: "600",
                  background: showVendorPicker ? "#FF950018" : "rgba(255,255,255,0.05)",
                  border: `1px solid ${showVendorPicker ? "#FF950055" : "rgba(255,255,255,0.1)"}`,
                  color: showVendorPicker ? "#FF9500" : "rgba(255,255,255,0.5)",
                  cursor: "pointer", flexShrink: 0
                }}>BROWSE</button>
            </div>

            {/* Match result banner */}
            {colorMatch && (
              <div style={{
                marginTop: "8px", display: "flex", alignItems: "center", gap: "10px",
                background: "rgba(127,255,0,0.06)", border: "1px solid rgba(127,255,0,0.2)",
                borderRadius: "8px", padding: "8px 12px"
              }}>
                <div style={{ width: "32px", height: "32px", borderRadius: "6px", background: colorMatch.hex_color, flexShrink: 0, border: "1px solid rgba(255,255,255,0.2)" }} />
                <div style={{ flex: 1 }}>
                  <div style={{ fontSize: "12px", color: "#7FFF00", fontWeight: "600" }}>{colorMatch.color_name}</div>
                  <div style={{ fontSize: "11px", color: "rgba(255,255,255,0.4)", fontFamily: "monospace" }}>{colorMatch.hex_color}
                    {colorMatch.td != null && <span style={{ marginLeft: "8px", color: "rgba(255,255,255,0.25)" }}>TD={colorMatch.td}</span>}
                  </div>
                </div>
                {colorMatch.image_card && (
                  <img src={colorMatch.image_card} alt="swatch" style={{ width: "44px", height: "44px", borderRadius: "6px", objectFit: "cover", border: "1px solid rgba(255,255,255,0.1)" }} />
                )}
                <button onClick={() => applyColorMatch(colorMatch.hex_color)}
                  style={{ padding: "7px 12px", borderRadius: "6px", fontSize: "11px", fontWeight: "700", background: "#7FFF0022", border: "1px solid #7FFF0066", color: "#7FFF00", cursor: "pointer" }}>
                  USE
                </button>
              </div>
            )}

            {colorLookupState === "not_found" && (
              <div style={{ marginTop: "6px", fontSize: "11px", color: "rgba(255,165,0,0.7)" }}>
                Not found in database - use color picker or BROWSE for all {form.vendor} colors.
              </div>
            )}

            {/* Vendor color browser */}
            {showVendorPicker && (
              <div style={{
                marginTop: "8px", maxHeight: "180px", overflowY: "auto",
                background: "rgba(255,255,255,0.03)", border: "1px solid rgba(255,255,255,0.08)",
                borderRadius: "8px", padding: "8px"
              }}>
                {vendorColors.length === 0
                  ? <div style={{ color: "rgba(255,255,255,0.3)", fontSize: "12px", padding: "8px" }}>Loading colors...</div>
                  : (
                    <div style={{ display: "flex", flexWrap: "wrap", gap: "6px" }}>
                      {vendorColors.map(c => (
                        <button key={c.id} onClick={() => { applyColorMatch(c.hex_color); set("name", c.color_name); }}
                          title={`${c.color_name} (${c.hex_color}) - ${c.filament_type}`}
                          style={{
                            width: "28px", height: "28px", borderRadius: "50%", border: "2px solid rgba(255,255,255,0.15)",
                            background: c.hex_color, cursor: "pointer", padding: 0, flexShrink: 0,
                            boxShadow: form.color_hex === c.hex_color ? `0 0 0 2px #00C9FF` : "none",
                            transition: "transform 0.1s"
                          }}
                          onMouseEnter={e => e.currentTarget.style.transform = "scale(1.25)"}
                          onMouseLeave={e => e.currentTarget.style.transform = "scale(1)"}
                        />
                      ))}
                    </div>
                  )
                }
              </div>
            )}
          </div>
          <div>
            <label style={labelStyle}>DIAMETER (mm)</label>
            <input style={fieldStyle} type="number" step="0.05" value={form.diameter} onChange={e => set("diameter", parseFloat(e.target.value))} />
          </div>
          <div>
            <label style={labelStyle}>TARE WEIGHT (g) <span style={{ color: "rgba(255,255,255,0.25)" }}>empty spool</span></label>
            <input style={fieldStyle} type="number" value={form.tare_weight} onChange={e => set("tare_weight", parseInt(e.target.value))} />
          </div>
          <div>
            <label style={labelStyle}>CURRENT GROSS WEIGHT (g)</label>
            <input style={fieldStyle} type="number" value={form.gross_weight} onChange={e => {
              const gw = parseInt(e.target.value);
              set("gross_weight", gw);
              set("remaining_weight", Math.max(0, gw - form.tare_weight));
            }} />
          </div>
          <div style={{ gridColumn: "1/-1" }}>
            <label style={labelStyle}>NFC TAG UID</label>
            <input style={fieldStyle} value={form.tag_uid} onChange={e => set("tag_uid", e.target.value)} placeholder="04:AB:12:CD:EF:01" />
          </div>
          <div>
            <label style={labelStyle}>TOOLHEAD (leave blank if storage)</label>
            <select style={fieldStyle} value={form.toolhead || ""} onChange={e => set("toolhead", e.target.value ? parseInt(e.target.value) : null)}>
              <option value="">- Not loaded -</option>
              <option value="1">T1</option>
              <option value="2">T2</option>
              <option value="3">T3</option>
              <option value="4">T4</option>
            </select>
          </div>
          <div style={{ display: "flex", flexDirection: "column", justifyContent: "flex-end" }}>
            <div style={{ background: "rgba(255,255,255,0.04)", borderRadius: "8px", padding: "10px 14px" }}>
              <div style={{ fontSize: "10px", color: "rgba(255,255,255,0.35)", letterSpacing: "0.08em" }}>CALCULATED REMAINING</div>
              <div style={{ fontSize: "20px", fontFamily: "monospace", color: "#00C9FF", fontWeight: "700", marginTop: "2px" }}>
                {remaining}g <span style={{ fontSize: "13px", color: "rgba(255,255,255,0.5)" }}>~ {lengthM}m</span>
              </div>
            </div>
          </div>
        </div>

        <div style={{ display: "flex", gap: "10px", marginTop: "24px" }}>
          <button onClick={onClose} style={{
            flex: 1, background: "rgba(255,255,255,0.05)", border: "1px solid rgba(255,255,255,0.1)",
            color: "rgba(255,255,255,0.6)", borderRadius: "8px", padding: "11px", cursor: "pointer", fontSize: "13px"
          }}>Cancel</button>
          <button onClick={() => onSave(form)} style={{
            flex: 2, background: "#00C9FF22", border: "1px solid #00C9FF55",
            color: "#00C9FF", borderRadius: "8px", padding: "11px", cursor: "pointer", fontSize: "13px", fontWeight: "600"
          }}>
            {isNew ? "Register Spool" : "Save Changes"}
          </button>
        </div>
      </div>
    </div>
  );
}

// --- QUICK REGISTER MODAL ----------------------------------------------------
// Shown when station reads a UID not matched to any spool in inventory.
// Pre-fills UID. Color auto-looks up once vendor+name are entered.
function QuickRegisterModal({ uid, weightG, onClose, onSave }) {
  const [form, setForm] = useState({
    tag_uid: uid,
    vendor: "Generic",
    material: "PLA",
    name: "",
    color_hex: "#AAAAAA",
    diameter: 1.75,
    tare_weight: 215,
    gross_weight: weightG || 0,
    remaining_weight: Math.max(0, (weightG || 0) - 215),
    toolhead: null,
  });
  const [lookupState, setLookupState] = useState("idle");
  const [colorMatch, setColorMatch] = useState(null);
  const [saveState, setSaveState] = useState("idle"); // idle | saving | ok | error
  const [tagWriteResult, setTagWriteResult] = useState(null); // "sent" | "offline_queued"
  const set = (k, v) => setForm(f => ({ ...f, [k]: v }));

  const handleLookup = async () => {
    if (!form.vendor || !form.name) return;
    setLookupState("loading");
    const result = await lookupFilamentColor(form.vendor, form.name, form.material);
    if (result) {
      setLookupState("found");
      setColorMatch(result);
    } else {
      setLookupState("not_found");
    }
  };

  // Recalculate remaining when tare changes
  const handleTareChange = (tare) => {
    set("tare_weight", tare);
    set("remaining_weight", Math.max(0, form.gross_weight - tare));
  };

  const fieldStyle = {
    width: "100%", background: "rgba(255,255,255,0.06)",
    border: "1px solid rgba(255,255,255,0.1)", borderRadius: "6px",
    color: "#F0F0F0", padding: "9px 12px", fontSize: "13px",
    fontFamily: "monospace", outline: "none", boxSizing: "border-box",
  };
  const label = (text, sub) => (
    <label style={{ fontSize: "10px", color: "rgba(255,255,255,0.4)", letterSpacing: "0.08em", marginBottom: "5px", display: "block" }}>
      {text}{sub && <span style={{ color: "rgba(255,255,255,0.2)", marginLeft: "6px" }}>{sub}</span>}
    </label>
  );

  const remaining = Math.max(0, form.gross_weight - form.tare_weight);
  const lengthM = calcRemainingLength(remaining, form.material, form.diameter);

  return (
    <div style={{
      position: "fixed", inset: 0, zIndex: 1100,
      background: "rgba(0,0,0,0.82)", backdropFilter: "blur(6px)",
      display: "flex", alignItems: "center", justifyContent: "center", padding: "20px",
    }} onClick={e => e.target === e.currentTarget && onClose()}>
      <div style={{
        background: "#111", border: "1px solid rgba(255,149,0,0.35)",
        borderRadius: "14px", padding: "28px", width: "100%", maxWidth: "500px",
        maxHeight: "90vh", overflowY: "auto",
        boxShadow: "0 0 40px rgba(255,149,0,0.12)",
      }}>
        {/* Header */}
        <div style={{ display: "flex", justifyContent: "space-between", alignItems: "flex-start", marginBottom: "20px" }}>
          <div>
            <div style={{ display: "flex", alignItems: "center", gap: "8px", marginBottom: "4px" }}>
              <div style={{ width: "8px", height: "8px", borderRadius: "50%", background: "#FF9500", animation: "pulse 1.5s infinite" }} />
              <span style={{ fontSize: "11px", color: "#FF9500", fontWeight: "700", letterSpacing: "0.1em" }}>UNKNOWN TAG DETECTED</span>
            </div>
            <div style={{ fontSize: "16px", fontWeight: "700", color: "#F0F0F0" }}>Register Spool</div>
          </div>
          <button onClick={onClose} style={{ background: "none", border: "none", color: "rgba(255,255,255,0.35)", cursor: "pointer", fontSize: "18px", lineHeight: 1 }}>x</button>
        </div>

        {/* UID + Weight read-only summary */}
        <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: "10px", marginBottom: "20px" }}>
          <div style={{ background: "rgba(255,149,0,0.07)", border: "1px solid rgba(255,149,0,0.2)", borderRadius: "8px", padding: "10px 14px" }}>
            <div style={{ fontSize: "9px", color: "rgba(255,255,255,0.35)", letterSpacing: "0.1em", marginBottom: "3px" }}>TAG UID</div>
            <div style={{ fontSize: "12px", fontFamily: "monospace", color: "#FF9500", wordBreak: "break-all" }}>{uid}</div>
          </div>
          <div style={{ background: "rgba(0,201,255,0.07)", border: "1px solid rgba(0,201,255,0.2)", borderRadius: "8px", padding: "10px 14px" }}>
            <div style={{ fontSize: "9px", color: "rgba(255,255,255,0.35)", letterSpacing: "0.1em", marginBottom: "3px" }}>SCALE READING</div>
            <div style={{ fontSize: "20px", fontFamily: "monospace", color: "#00C9FF", fontWeight: "700" }}>
              {weightG ? `${weightG}g` : "-"}
            </div>
          </div>
        </div>

        {/* Form */}
        <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: "13px" }}>
          {/* Spool name - blur triggers color lookup */}
          <div style={{ gridColumn: "1/-1" }}>
            {label("SPOOL NAME / COLOR NAME")}
            <input style={fieldStyle} value={form.name}
              onChange={e => set("name", e.target.value)}
              onBlur={handleLookup}
              placeholder="e.g. Galaxy Purple - tab to auto-lookup color" />
          </div>

          <div>
            {label("VENDOR")}
            <select style={fieldStyle} value={form.vendor}
              onChange={e => { set("vendor", e.target.value); setColorMatch(null); setLookupState("idle"); }}>
              {["Prusament","Bambu Lab","Kingroon","eSun","Polymaker","Sunlu","Hatchbox","OVERTURE","Snapmaker","Generic"].map(v => <option key={v}>{v}</option>)}
            </select>
          </div>

          <div>
            {label("MATERIAL")}
            <select style={fieldStyle} value={form.material} onChange={e => set("material", e.target.value)}>
              {Object.keys(MATERIAL_DENSITIES).map(m => <option key={m}>{m}</option>)}
            </select>
          </div>

          {/* Color row */}
          <div style={{ gridColumn: "1/-1" }}>
            {label("COLOR", colorMatch ? "- matched from filamentcolors.xyz" : "")}
            <div style={{ display: "flex", gap: "8px", alignItems: "center" }}>
              <input type="color" value={form.color_hex} onChange={e => set("color_hex", e.target.value)}
                style={{ width: "40px", height: "37px", borderRadius: "6px", border: "1px solid rgba(255,255,255,0.1)", background: "none", padding: "2px", cursor: "pointer", flexShrink: 0 }} />
              <input style={{ ...fieldStyle, flex: 1 }} value={form.color_hex} onChange={e => set("color_hex", e.target.value)} />
              <button onClick={handleLookup} disabled={lookupState === "loading"}
                style={{
                  padding: "9px 12px", borderRadius: "6px", fontSize: "11px", fontWeight: "600",
                  background: lookupState === "found" ? "#7FFF0018" : "#00C9FF18",
                  border: `1px solid ${lookupState === "found" ? "#7FFF0055" : "#00C9FF44"}`,
                  color: lookupState === "found" ? "#7FFF00" : "#00C9FF",
                  cursor: "pointer", flexShrink: 0, whiteSpace: "nowrap",
                }}>
                {lookupState === "loading" ? "..." : lookupState === "found" ? "v MATCHED" : "LOOKUP"}
              </button>
            </div>
            {colorMatch && (
              <div style={{ marginTop: "8px", display: "flex", alignItems: "center", gap: "10px",
                background: "rgba(127,255,0,0.06)", border: "1px solid rgba(127,255,0,0.2)",
                borderRadius: "7px", padding: "7px 11px" }}>
                <div style={{ width: "28px", height: "28px", borderRadius: "5px", background: colorMatch.hex_color, flexShrink: 0, border: "1px solid rgba(255,255,255,0.15)" }} />
                <div style={{ flex: 1 }}>
                  <div style={{ fontSize: "12px", color: "#7FFF00", fontWeight: "600" }}>{colorMatch.color_name}</div>
                  <div style={{ fontSize: "10px", color: "rgba(255,255,255,0.35)", fontFamily: "monospace" }}>{colorMatch.hex_color}</div>
                </div>
                {colorMatch.image_card && <img src={colorMatch.image_card} alt="" style={{ width: "40px", height: "40px", borderRadius: "5px", objectFit: "cover" }} />}
                <button onClick={() => set("color_hex", colorMatch.hex_color)}
                  style={{ padding: "6px 10px", borderRadius: "5px", fontSize: "11px", fontWeight: "700",
                    background: "#7FFF0022", border: "1px solid #7FFF0066", color: "#7FFF00", cursor: "pointer" }}>USE</button>
              </div>
            )}
          </div>

          {/* Tare + gross */}
          <div>
            {label("TARE WEIGHT (g)", "empty spool")}
            <input style={fieldStyle} type="number" value={form.tare_weight}
              onChange={e => handleTareChange(parseInt(e.target.value) || 0)} />
          </div>
          <div>
            {label("GROSS WEIGHT (g)", "from scale")}
            <input style={fieldStyle} type="number" value={form.gross_weight}
              onChange={e => {
                const gw = parseInt(e.target.value) || 0;
                set("gross_weight", gw);
                set("remaining_weight", Math.max(0, gw - form.tare_weight));
              }} />
          </div>

          {/* Calculated remaining */}
          <div style={{ gridColumn: "1/-1" }}>
            <div style={{ background: "rgba(255,255,255,0.04)", borderRadius: "8px", padding: "10px 14px", display: "flex", justifyContent: "space-between", alignItems: "center" }}>
              <div>
                <div style={{ fontSize: "9px", color: "rgba(255,255,255,0.3)", letterSpacing: "0.08em" }}>CALCULATED REMAINING FILAMENT</div>
                <div style={{ fontSize: "22px", fontFamily: "monospace", color: "#00C9FF", fontWeight: "700", marginTop: "2px" }}>
                  {remaining}g <span style={{ fontSize: "13px", color: "rgba(255,255,255,0.4)" }}>~ {lengthM}m</span>
                </div>
              </div>
              <div style={{ fontSize: "11px", color: "rgba(255,255,255,0.25)", textAlign: "right" }}>
                <div>diameter: {form.diameter}mm</div>
                <div>density: {MATERIAL_DENSITIES[form.material] || 1.24} g/cm³</div>
              </div>
            </div>
          </div>
        </div>

        {/* Tag write result banner */}
        {tagWriteResult && (
          <div style={{
            marginTop: "12px", padding: "9px 14px", borderRadius: "8px",
            background: tagWriteResult === "sent" ? "rgba(127,255,0,0.07)" : "rgba(255,149,0,0.07)",
            border: `1px solid ${tagWriteResult === "sent" ? "rgba(127,255,0,0.25)" : "rgba(255,149,0,0.25)"}`,
            display: "flex", alignItems: "center", gap: "10px"
          }}>
            <span style={{ fontSize: "18px" }}>{tagWriteResult === "sent" ? "" : "(!!)"}</span>
            <div>
              <div style={{ fontSize: "12px", fontWeight: "600", color: tagWriteResult === "sent" ? "#7FFF00" : "#FF9500" }}>
                {tagWriteResult === "sent" ? "Tag written successfully" : "Tag write queued - station offline"}
              </div>
              <div style={{ fontSize: "11px", color: "rgba(255,255,255,0.35)", marginTop: "2px" }}>
                {tagWriteResult === "sent"
                  ? "OpenSpool JSON written to tag. Spool registered and ready."
                  : "Spool registered in Spoolman. Write will fire when station reconnects."}
              </div>
            </div>
          </div>
        )}

        {/* Actions */}
        <div style={{ display: "flex", gap: "10px", marginTop: "16px" }}>
          <button onClick={onClose} disabled={saveState === "saving"} style={{
            flex: 1, padding: "11px", borderRadius: "8px", fontSize: "13px",
            background: "rgba(255,255,255,0.04)", border: "1px solid rgba(255,255,255,0.08)",
            color: saveState === "saving" ? "rgba(255,255,255,0.2)" : "rgba(255,255,255,0.5)",
            cursor: saveState === "saving" ? "not-allowed" : "pointer",
          }}>{tagWriteResult ? "Close" : "Skip for now"}</button>

          {!tagWriteResult && (
            <button disabled={saveState === "saving"} onClick={async () => {
              setSaveState("saving");
              try {
                const body = {
                  tag_uid: form.tag_uid,
                  vendor: form.vendor,
                  material: form.material,
                  name: form.name,
                  color_hex: form.color_hex.replace("#", ""),
                  diameter: form.diameter,
                  tare_weight: form.tare_weight,
                  gross_weight: form.gross_weight,
                  min_temp: form.min_temp || 200,
                  max_temp: form.max_temp || 230,
                  bed_min_temp: form.bed_min_temp || 50,
                  bed_max_temp: form.bed_max_temp || 65,
                };
                const r = await fetch(`${BRIDGE_BASE}/register-and-write`, {
                  method: "POST",
                  headers: { "Content-Type": "application/json" },
                  body: JSON.stringify(body),
                });
                const data = await r.json();
                if (r.ok && data.status === "ok") {
                  setSaveState("ok");
                  setTagWriteResult(data.tag_write); // "sent" or "offline_queued"
                  onSave({
                    ...form,
                    id: data.spool_id,
                    remaining_weight: data.remaining_weight,
                    remaining_length_m: data.remaining_length_m,
                  });
                } else {
                  setSaveState("error");
                  console.error("Register failed:", data);
                }
              } catch (e) {
                setSaveState("error");
                console.error("Register error:", e);
              }
            }} style={{
              flex: 2, padding: "11px", borderRadius: "8px", fontSize: "13px", fontWeight: "700",
              background: saveState === "error" ? "#FF3B6B22" : saveState === "saving" ? "rgba(255,255,255,0.05)" : "#FF950022",
              border: `1px solid ${saveState === "error" ? "#FF3B6B66" : saveState === "saving" ? "rgba(255,255,255,0.1)" : "#FF950066"}`,
              color: saveState === "error" ? "#FF3B6B" : saveState === "saving" ? "rgba(255,255,255,0.3)" : "#FF9500",
              cursor: saveState === "saving" ? "wait" : "pointer",
              display: "flex", alignItems: "center", justifyContent: "center", gap: "8px",
            }}>
              {saveState === "saving" && (
                <span style={{ display: "inline-block", animation: "spin 1s linear infinite", fontSize: "14px" }}></span>
              )}
              {saveState === "saving" ? "Registering..." : saveState === "error" ? "Failed - Retry" : "Register + Write Tag"}
            </button>
          )}
        </div>
      </div>
    </div>
  );
}

// --- MAIN DASHBOARD -----------------------------------------------------------
export default function FilamentStation() {
  const [tab, setTab] = useState("inventory");
  const [spools, setSpools] = useState(MOCK_SPOOLS);
  const [log, setLog] = useState(MOCK_LOG);
  const [search, setSearch] = useState("");
  const [filterMaterial, setFilterMaterial] = useState("ALL");
  const [filterToolhead, setFilterToolhead] = useState("ALL");
  const [modalSpool, setModalSpool] = useState(null);
  const [modalOpen, setModalOpen] = useState(false);
  const [stationStatus, setStationStatus] = useState({ connected: true, weight: null, tagUid: null, scanning: false });
  const [unknownTag, setUnknownTag] = useState(null); // {uid, weight_g} - triggers quick-register modal
  const [u1SyncState, setU1SyncState] = useState("idle");   // idle | syncing | ok | error
  const [u1LastSync, setU1LastSync] = useState(null);        // ISO timestamp
  const [u1Changes, setU1Changes] = useState([]);            // recent changes from last sync

  const handleU1Sync = async () => {
    setU1SyncState("syncing");
    try {
      const r = await fetch(`${BRIDGE_BASE}/u1/sync`, { method: "POST" });
      const data = await r.json();
      if (data.status === "ok") {
        setU1SyncState("ok");
        setU1LastSync(data.polled_at);
        setU1Changes(data.changes || []);
        // Auto-reset to idle after 3s
        setTimeout(() => setU1SyncState("idle"), 3000);
      } else {
        setU1SyncState("error");
        setTimeout(() => setU1SyncState("idle"), 4000);
      }
    } catch {
      setU1SyncState("error");
      setTimeout(() => setU1SyncState("idle"), 4000);
    }
  };

  // Auto-poll every 30s
  useEffect(() => {
    const interval = setInterval(handleU1Sync, 30000);
    return () => clearInterval(interval);
  }, []);

  // Poll bridge /status every 2s for live scale + tag data
  useEffect(() => {
    const poll = async () => {
      try {
        const r = await fetch(`${BRIDGE_BASE}/status`);
        if (!r.ok) return;
        const data = await r.json();
        setStationStatus(s => ({
          ...s,
          connected: data.connected ?? s.connected,
          weight: data.last_weight_g ?? s.weight,
          tagUid: data.last_tag_uid ?? s.tagUid,
        }));

        // Unknown tag detected - not in any spool record
        const uid = data.last_tag_uid;
        if (uid) {
          const known = spools.some(s => (s.tag_uid || "").toUpperCase() === uid.toUpperCase());
          if (!known) {
            setUnknownTag({ uid, weight_g: data.last_weight_g });
          } else {
            setUnknownTag(null);
          }
        } else {
          setUnknownTag(null);
        }
      } catch {
        setStationStatus(s => ({ ...s, connected: false }));
      }
    };
    poll();
    const interval = setInterval(poll, 2000);
    return () => clearInterval(interval);
  }, [spools]);

  const filtered = spools.filter(s => {
    const q = search.toLowerCase();
    const matchSearch = !q || s.name.toLowerCase().includes(q) || s.vendor.toLowerCase().includes(q) || s.material.toLowerCase().includes(q);
    const matchMaterial = filterMaterial === "ALL" || s.material === filterMaterial;
    const matchToolhead = filterToolhead === "ALL"
      ? true
      : filterToolhead === "LOADED" ? !!s.toolhead
      : filterToolhead === "STORAGE" ? !s.toolhead
      : false;
    return matchSearch && matchMaterial && matchToolhead;
  });

  const materials = ["ALL", ...Array.from(new Set(spools.map(s => s.material)))];
  const totalSpools = spools.length;
  const loadedSpools = spools.filter(s => s.toolhead).length;
  const lowSpools = spools.filter(s => s.remaining_weight < 100).length;
  const totalFilamentKg = (spools.reduce((acc, s) => acc + s.remaining_weight, 0) / 1000).toFixed(2);

  const tabStyle = (t) => ({
    padding: "8px 16px", borderRadius: "7px", cursor: "pointer", fontSize: "13px",
    fontWeight: t === tab ? "600" : "400",
    color: t === tab ? "#F0F0F0" : "rgba(255,255,255,0.4)",
    background: t === tab ? "rgba(255,255,255,0.08)" : "transparent",
    border: "none", display: "flex", alignItems: "center", gap: "7px",
    transition: "all 0.15s ease"
  });

  return (
    <div style={{
      minHeight: "100vh", background: "#0D0D0D",
      color: "#F0F0F0", fontFamily: "'DM Mono', 'Fira Code', 'Cascadia Code', monospace",
    }}>
      <style>{`
        @import url('https://fonts.googleapis.com/css2?family=DM+Mono:wght@300;400;500&family=DM+Sans:wght@300;400;500;600;700&display=swap');
        * { box-sizing: border-box; }
        ::-webkit-scrollbar { width: 5px; height: 5px; }
        ::-webkit-scrollbar-track { background: rgba(255,255,255,0.03); }
        ::-webkit-scrollbar-thumb { background: rgba(255,255,255,0.15); border-radius: 3px; }
        input::placeholder { color: rgba(255,255,255,0.2); }
        input:focus, select:focus { outline: 1px solid rgba(0,201,255,0.4); }
        select option { background: #1a1a1a; color: #f0f0f0; }
        @keyframes pulse { 0%,100%{opacity:1} 50%{opacity:0.4} }
        @keyframes slideIn { from{opacity:0;transform:translateY(8px)} to{opacity:1;transform:translateY(0)} }
        @keyframes spin { from{transform:rotate(0deg)} to{transform:rotate(360deg)} }
      `}</style>

      {/* -- HEADER -- */}
      <header style={{
        borderBottom: "1px solid rgba(255,255,255,0.07)",
        padding: "0 32px", height: "58px",
        display: "flex", alignItems: "center", justifyContent: "space-between",
        background: "rgba(0,0,0,0.4)", backdropFilter: "blur(12px)",
        position: "sticky", top: 0, zIndex: 100
      }}>
        <div style={{ display: "flex", alignItems: "center", gap: "14px" }}>
          <div style={{ display: "flex", alignItems: "center", gap: "8px" }}>
            <div style={{ width: "28px", height: "28px", borderRadius: "7px", background: "linear-gradient(135deg, #00C9FF, #0065B3)", display: "flex", alignItems: "center", justifyContent: "center" }}>
              <IconSpool />
            </div>
            <span style={{ fontSize: "14px", fontWeight: "700", letterSpacing: "0.05em", color: "#F0F0F0" }}>SPOOLDESK</span>
          </div>
          <div style={{ width: "1px", height: "20px", background: "rgba(255,255,255,0.1)" }} />
          <nav style={{ display: "flex", gap: "4px" }}>
            {[
              { id: "inventory", icon: <IconSpool />, label: "Inventory" },
              { id: "station", icon: <IconScale />, label: "Station" },
              { id: "toolheads", icon: <IconPrinter />, label: "Toolheads" },
              { id: "log", icon: <IconActivity />, label: "Activity" },
            ].map(({ id, icon, label }) => (
              <button key={id} onClick={() => setTab(id)} style={tabStyle(id)}>
                {icon}{label}
              </button>
            ))}
          </nav>
        </div>
        <div style={{ display: "flex", alignItems: "center", gap: "12px" }}>
          <div style={{ display: "flex", alignItems: "center", gap: "6px", fontSize: "11px" }}>
            <div style={{
              width: "7px", height: "7px", borderRadius: "50%",
              background: stationStatus.connected ? "#7FFF00" : "#FF3B6B",
              animation: stationStatus.connected ? "pulse 2s infinite" : "none"
            }} />
            <span style={{ color: "rgba(255,255,255,0.4)" }}>
              {stationStatus.connected ? "Station Online" : "Station Offline"}
            </span>
          </div>
          <button onClick={handleU1Sync} disabled={u1SyncState === "syncing"} title={u1LastSync ? `Last synced: ${new Date(u1LastSync).toLocaleTimeString()}` : "Sync U1 toolhead state"}
            style={{
              background: u1SyncState === "ok" ? "#7FFF0018" : u1SyncState === "error" ? "#FF3B6B18" : "rgba(255,255,255,0.05)",
              border: `1px solid ${u1SyncState === "ok" ? "#7FFF0055" : u1SyncState === "error" ? "#FF3B6B55" : "rgba(255,255,255,0.1)"}`,
              color: u1SyncState === "ok" ? "#7FFF00" : u1SyncState === "error" ? "#FF3B6B" : "rgba(255,255,255,0.5)",
              borderRadius: "7px", padding: "7px 14px", cursor: u1SyncState === "syncing" ? "wait" : "pointer",
              fontSize: "12px", fontWeight: "600", display: "flex", alignItems: "center", gap: "6px",
              transition: "all 0.2s"
            }}>
            <span style={{ display: "inline-block", animation: u1SyncState === "syncing" ? "spin 1s linear infinite" : "none" }}>
              <IconSync />
            </span>
            {u1SyncState === "syncing" ? "SYNCING..." : u1SyncState === "ok" ? "SYNCED" : u1SyncState === "error" ? "SYNC FAILED" : "SYNC U1"}
          </button>
          <button onClick={() => { setModalSpool(null); setModalOpen(true); }} style={{
            background: "#00C9FF18", border: "1px solid #00C9FF44",
            color: "#00C9FF", borderRadius: "7px", padding: "7px 14px",
            cursor: "pointer", fontSize: "12px", fontWeight: "600",
            display: "flex", alignItems: "center", gap: "6px"
          }}>
            <IconPlus />NEW SPOOL
          </button>
        </div>
      </header>

      <main style={{ padding: "28px 32px", maxWidth: "1400px", margin: "0 auto" }}>

        {/* -- STATS ROW -- */}
        <div style={{ display: "grid", gridTemplateColumns: "repeat(4, 1fr)", gap: "14px", marginBottom: "28px", animation: "slideIn 0.4s ease" }}>
          {[
            { label: "TOTAL SPOOLS", value: totalSpools, color: "#00C9FF", sub: `${loadedSpools} loaded` },
            { label: "TOTAL FILAMENT", value: `${totalFilamentKg}kg`, color: "#7FFF00", sub: "remaining" },
            { label: "LOW STOCK", value: lowSpools, color: lowSpools > 0 ? "#FF3B6B" : "#7FFF00", sub: "< 100g remaining" },
            { label: "LOADED", value: `${loadedSpools}/4`, color: "#FF9500", sub: "toolheads active" },
          ].map(({ label, value, color, sub }) => (
            <div key={label} style={{
              background: "rgba(255,255,255,0.033)", border: "1px solid rgba(255,255,255,0.07)",
              borderRadius: "10px", padding: "18px 20px"
            }}>
              <div style={{ fontSize: "10px", color: "rgba(255,255,255,0.35)", letterSpacing: "0.1em", marginBottom: "8px" }}>{label}</div>
              <div style={{ fontSize: "28px", fontWeight: "700", color, lineHeight: 1 }}>{value}</div>
              <div style={{ fontSize: "11px", color: "rgba(255,255,255,0.3)", marginTop: "5px" }}>{sub}</div>
            </div>
          ))}
        </div>

        {/* -- INVENTORY TAB -- */}
        {tab === "inventory" && (
          <div style={{ animation: "slideIn 0.3s ease" }}>
            {/* Filters */}
            <div style={{ display: "flex", gap: "10px", marginBottom: "20px", flexWrap: "wrap" }}>
              <div style={{ position: "relative", flex: "1", minWidth: "200px" }}>
                <span style={{ position: "absolute", left: "12px", top: "50%", transform: "translateY(-50%)", color: "rgba(255,255,255,0.3)" }}>
                  <IconSearch />
                </span>
                <input
                  style={{
                    width: "100%", paddingLeft: "36px", paddingRight: "12px",
                    background: "rgba(255,255,255,0.05)", border: "1px solid rgba(255,255,255,0.08)",
                    borderRadius: "8px", color: "#F0F0F0", height: "38px", fontSize: "13px",
                    fontFamily: "inherit"
                  }}
                  placeholder="Search spools..."
                  value={search}
                  onChange={e => setSearch(e.target.value)}
                />
              </div>
              {materials.map(m => (
                <button key={m} onClick={() => setFilterMaterial(m)} style={{
                  padding: "0 14px", height: "38px", borderRadius: "8px", fontSize: "12px", fontWeight: "600",
                  cursor: "pointer", border: `1px solid ${filterMaterial === m ? "#00C9FF55" : "rgba(255,255,255,0.08)"}`,
                  background: filterMaterial === m ? "#00C9FF18" : "rgba(255,255,255,0.03)",
                  color: filterMaterial === m ? "#00C9FF" : "rgba(255,255,255,0.5)",
                  letterSpacing: "0.05em", transition: "all 0.15s"
                }}>{m}</button>
              ))}
              <div style={{ display: "flex", gap: "6px" }}>
                {[{ v: "ALL", label: "All" }, { v: "LOADED", label: "Loaded" }, { v: "STORAGE", label: "Storage" }].map(({ v, label }) => (
                  <button key={v} onClick={() => setFilterToolhead(v)} style={{
                    padding: "0 12px", height: "38px", borderRadius: "8px", fontSize: "12px",
                    cursor: "pointer", border: `1px solid ${filterToolhead === v ? "#FF950055" : "rgba(255,255,255,0.08)"}`,
                    background: filterToolhead === v ? "#FF950018" : "rgba(255,255,255,0.03)",
                    color: filterToolhead === v ? "#FF9500" : "rgba(255,255,255,0.5)",
                    transition: "all 0.15s"
                  }}>{label}</button>
                ))}
              </div>
            </div>

            <div style={{ display: "grid", gridTemplateColumns: "repeat(auto-fill, minmax(280px, 1fr))", gap: "14px" }}>
              {filtered.map(s => (
                <SpoolCard key={s.id} spool={s} onEdit={(sp) => { setModalSpool(sp); setModalOpen(true); }} />
              ))}
              {filtered.length === 0 && (
                <div style={{ gridColumn: "1/-1", textAlign: "center", color: "rgba(255,255,255,0.25)", padding: "48px", fontSize: "14px" }}>
                  No spools found
                </div>
              )}
            </div>
          </div>
        )}

        {/* -- STATION TAB -- */}
        {tab === "station" && (
          <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: "20px", animation: "slideIn 0.3s ease" }}>
            {/* Live Scale */}
            <div style={{ background: "rgba(255,255,255,0.033)", border: "1px solid rgba(255,255,255,0.07)", borderRadius: "12px", padding: "24px" }}>
              <div style={{ fontSize: "11px", color: "rgba(255,255,255,0.4)", letterSpacing: "0.1em", marginBottom: "16px" }}>SCALE READING</div>
              <div style={{ textAlign: "center", padding: "32px 0" }}>
                <div style={{
                  fontSize: "72px", fontWeight: "700", lineHeight: 1,
                  color: stationStatus.weight ? "#00C9FF" : "rgba(255,255,255,0.15)",
                  textShadow: stationStatus.weight ? "0 0 40px #00C9FF44" : "none",
                  transition: "all 0.3s"
                }}>
                  {stationStatus.weight ? `${stationStatus.weight}` : "-"}
                </div>
                <div style={{ fontSize: "18px", color: "rgba(255,255,255,0.4)", marginTop: "6px" }}>grams</div>
              </div>
              <div style={{ display: "flex", gap: "10px" }}>
                <button
                  onClick={() => setStationStatus(s => ({ ...s, scanning: !s.scanning }))}
                  style={{
                    flex: 1, padding: "12px", borderRadius: "8px", fontSize: "13px", fontWeight: "600",
                    background: stationStatus.scanning ? "#FF3B6B18" : "#00C9FF18",
                    border: `1px solid ${stationStatus.scanning ? "#FF3B6B55" : "#00C9FF55"}`,
                    color: stationStatus.scanning ? "#FF3B6B" : "#00C9FF",
                    cursor: "pointer", transition: "all 0.2s"
                  }}>
                  {stationStatus.scanning ? "[ ] STOP" : "~ TARE / START"}
                </button>
              </div>
            </div>

            {/* NFC Reader */}
            <div style={{ background: "rgba(255,255,255,0.033)", border: "1px solid rgba(255,255,255,0.07)", borderRadius: "12px", padding: "24px" }}>
              <div style={{ fontSize: "11px", color: "rgba(255,255,255,0.4)", letterSpacing: "0.1em", marginBottom: "16px" }}>NFC READER</div>
              <div style={{ textAlign: "center", padding: "24px 0" }}>
                <div style={{
                  width: "80px", height: "80px", borderRadius: "50%", margin: "0 auto 16px",
                  background: stationStatus.tagUid ? "#7FFF0022" : "rgba(255,255,255,0.05)",
                  border: `2px solid ${stationStatus.tagUid ? "#7FFF00" : "rgba(255,255,255,0.1)"}`,
                  display: "flex", alignItems: "center", justifyContent: "center",
                  fontSize: "32px", animation: stationStatus.scanning ? "pulse 1.5s infinite" : "none"
                }}>
                  <IconRFID />
                </div>
                {stationStatus.tagUid ? (
                  <div>
                    <div style={{ fontSize: "13px", color: "#7FFF00", fontFamily: "monospace" }}>{stationStatus.tagUid}</div>
                    <div style={{ fontSize: "11px", color: "rgba(255,255,255,0.35)", marginTop: "4px" }}>Tag detected</div>
                  </div>
                ) : (
                  <div style={{ fontSize: "13px", color: "rgba(255,255,255,0.3)" }}>
                    {stationStatus.scanning ? "Waiting for tag..." : "Place spool on station"}
                  </div>
                )}
              </div>
              <div style={{ background: "rgba(255,255,255,0.04)", borderRadius: "8px", padding: "12px 14px" }}>
                <div style={{ fontSize: "10px", color: "rgba(255,255,255,0.3)", letterSpacing: "0.08em", marginBottom: "8px" }}>WRITE NEW TAG</div>
                <div style={{ display: "flex", gap: "8px" }}>
                  <select id="write-tag-select" style={{
                    flex: 1, background: "rgba(255,255,255,0.06)", border: "1px solid rgba(255,255,255,0.1)",
                    borderRadius: "6px", color: "#F0F0F0", padding: "8px 10px", fontSize: "12px", fontFamily: "inherit"
                  }}>
                    {spools.map(s => <option key={s.id} value={s.id}>{s.vendor} - {s.name} ({s.remaining_weight}g)</option>)}
                  </select>
                  <button onClick={async () => {
                    const sel = document.getElementById("write-tag-select");
                    if (!sel || !sel.value) return;
                    const r = await fetch(`${BRIDGE_BASE}/write-tag`, {
                      method: "POST",
                      headers: { "Content-Type": "application/json" },
                      body: JSON.stringify({ spool_id: parseInt(sel.value) }),
                    });
                    const d = await r.json();
                    alert(d.status === "queued"
                      ? "Tag write queued - present tag to reader"
                      : d.status === "queued_offline"
                      ? "Station offline - will write when reconnected"
                      : "Error: " + JSON.stringify(d));
                  }} style={{
                    padding: "8px 14px", borderRadius: "6px", fontSize: "12px", fontWeight: "600",
                    background: "#7FFF0018", border: "1px solid #7FFF0055", color: "#7FFF00", cursor: "pointer"
                  }}>WRITE</button>
                </div>
              </div>
            </div>

            {/* Quick Log */}
            <div style={{ gridColumn: "1/-1", background: "rgba(255,255,255,0.033)", border: "1px solid rgba(255,255,255,0.07)", borderRadius: "12px", padding: "24px" }}>
              <div style={{ fontSize: "11px", color: "rgba(255,255,255,0.4)", letterSpacing: "0.1em", marginBottom: "16px" }}>LAST WEIGHED SPOOL</div>
              {log[0] ? (
                <div style={{ display: "grid", gridTemplateColumns: "repeat(5, 1fr)", gap: "14px" }}>
                  {[
                    { label: "SPOOL", value: `${log[0].vendor} ${log[0].name}` },
                    { label: "PREVIOUS", value: `${log[0].old_weight}g` },
                    { label: "NEW WEIGHT", value: `${log[0].new_weight}g` },
                    { label: "DELTA", value: `${log[0].delta}g`, color: "#FF9500" },
                    { label: "TIMESTAMP", value: new Date(log[0].ts).toLocaleTimeString() },
                  ].map(({ label, value, color }) => (
                    <div key={label} style={{ background: "rgba(255,255,255,0.04)", borderRadius: "8px", padding: "12px 14px" }}>
                      <div style={{ fontSize: "10px", color: "rgba(255,255,255,0.35)", letterSpacing: "0.08em", marginBottom: "4px" }}>{label}</div>
                      <div style={{ fontSize: "14px", fontFamily: "monospace", color: color || "#E0E0E0", fontWeight: "600" }}>{value}</div>
                    </div>
                  ))}
                </div>
              ) : <div style={{ color: "rgba(255,255,255,0.3)", fontSize: "13px" }}>No readings yet</div>}
            </div>
          </div>
        )}

        {/* -- TOOLHEADS TAB -- */}
        {tab === "toolheads" && (
          <div style={{ animation: "slideIn 0.3s ease" }}>
            {/* Live sync status bar */}
            <div style={{ display: "flex", alignItems: "center", justifyContent: "space-between", marginBottom: "16px", padding: "10px 16px", background: "rgba(255,255,255,0.025)", borderRadius: "8px", border: "1px solid rgba(255,255,255,0.06)" }}>
              <div style={{ fontSize: "12px", color: "rgba(255,255,255,0.4)" }}>
                {u1LastSync ? `Last synced with U1: ${new Date(u1LastSync).toLocaleTimeString()}` : "Not yet synced with U1"}
                <span style={{ marginLeft: "10px", fontSize: "11px", color: "rgba(255,255,255,0.2)" }}>auto-polls every 30s</span>
              </div>
              <button onClick={handleU1Sync} disabled={u1SyncState === "syncing"}
                style={{
                  padding: "6px 14px", borderRadius: "6px", fontSize: "11px", fontWeight: "600",
                  background: u1SyncState === "syncing" ? "rgba(255,255,255,0.05)" : "#00C9FF18",
                  border: `1px solid ${u1SyncState === "syncing" ? "rgba(255,255,255,0.1)" : "#00C9FF44"}`,
                  color: u1SyncState === "syncing" ? "rgba(255,255,255,0.3)" : "#00C9FF",
                  cursor: u1SyncState === "syncing" ? "wait" : "pointer",
                  display: "flex", alignItems: "center", gap: "6px"
                }}>
                <span style={{ display: "inline-block", animation: u1SyncState === "syncing" ? "spin 1s linear infinite" : "none" }}><IconSync /></span>
                {u1SyncState === "syncing" ? "Syncing..." : "Sync Now"}
              </button>
            </div>
            <div style={{ display: "grid", gridTemplateColumns: "repeat(4, 1fr)", gap: "16px", marginBottom: "24px" }}>
              {[1, 2, 3, 4].map(t => {
                const loaded = spools.find(s => s.toolhead === t);
                const color = TOOLHEAD_COLORS[t];
                return (
                  <div key={t} style={{
                    background: "rgba(255,255,255,0.033)",
                    border: `1px solid ${loaded ? color + "44" : "rgba(255,255,255,0.07)"}`,
                    borderRadius: "12px", padding: "20px", position: "relative", overflow: "hidden"
                  }}>
                    <div style={{ position: "absolute", top: 0, left: 0, right: 0, height: "2px", background: color, opacity: loaded ? 1 : 0.2 }} />
                    <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: "16px" }}>
                      <div style={{ fontSize: "22px", fontWeight: "800", color: loaded ? color : "rgba(255,255,255,0.2)" }}>T{t}</div>
                      {loaded && (
                        <div style={{ width: "12px", height: "12px", borderRadius: "50%", background: "#7FFF00", boxShadow: "0 0 8px #7FFF0066" }} />
                      )}
                    </div>
                    {loaded ? (
                      <>
                        <div style={{ display: "flex", alignItems: "center", gap: "8px", marginBottom: "10px" }}>
                          <div style={{ width: "20px", height: "20px", borderRadius: "50%", background: loaded.color_hex, border: "1px solid rgba(255,255,255,0.2)", flexShrink: 0 }} />
                          <div>
                            <div style={{ fontSize: "13px", fontWeight: "600", color: "#F0F0F0" }}>{loaded.name}</div>
                            <div style={{ fontSize: "11px", color: "rgba(255,255,255,0.4)" }}>{loaded.vendor} · {loaded.material}</div>
                          </div>
                        </div>
                        <FilamentBar remaining={loaded.remaining_weight} total={loaded.gross_weight - loaded.tare_weight} color={color} />
                        <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: "8px", marginTop: "12px" }}>
                          <div style={{ background: "rgba(255,255,255,0.04)", borderRadius: "6px", padding: "8px 10px" }}>
                            <div style={{ fontSize: "9px", color: "rgba(255,255,255,0.35)", letterSpacing: "0.08em" }}>REMAINING</div>
                            <div style={{ fontSize: "15px", fontFamily: "monospace", color: "#E0E0E0", fontWeight: "600", marginTop: "2px" }}>{loaded.remaining_weight}g</div>
                          </div>
                          <div style={{ background: "rgba(255,255,255,0.04)", borderRadius: "6px", padding: "8px 10px" }}>
                            <div style={{ fontSize: "9px", color: "rgba(255,255,255,0.35)", letterSpacing: "0.08em" }}>LENGTH</div>
                            <div style={{ fontSize: "15px", fontFamily: "monospace", color: "#E0E0E0", fontWeight: "600", marginTop: "2px" }}>
                              ~{calcRemainingLength(loaded.remaining_weight, loaded.material, loaded.diameter)}m
                            </div>
                          </div>
                        </div>
                        <button onClick={() => {
                          setSpools(prev => prev.map(s => s.id === loaded.id ? { ...s, toolhead: null } : s));
                        }} style={{
                          marginTop: "12px", width: "100%", padding: "8px", borderRadius: "6px", fontSize: "11px",
                          background: "rgba(255,59,107,0.08)", border: "1px solid rgba(255,59,107,0.25)",
                          color: "#FF3B6B", cursor: "pointer", fontWeight: "600", letterSpacing: "0.05em"
                        }}>UNLOAD</button>
                      </>
                    ) : (
                      <div style={{ textAlign: "center", padding: "20px 0" }}>
                        <div style={{ fontSize: "13px", color: "rgba(255,255,255,0.2)", marginBottom: "12px" }}>Empty</div>
                        <select
                          onChange={e => {
                            if (!e.target.value) return;
                            setSpools(prev => prev.map(s => s.id === parseInt(e.target.value) ? { ...s, toolhead: t } : s.toolhead === t ? { ...s, toolhead: null } : s));
                            e.target.value = "";
                          }}
                          style={{ ...{ background: "rgba(255,255,255,0.05)", border: `1px solid ${color}33`, borderRadius: "6px", color: color, padding: "8px 10px", fontSize: "12px", cursor: "pointer", width: "100%", fontFamily: "inherit" } }}>
                          <option value="">Load spool...</option>
                          {spools.filter(s => !s.toolhead).map(s => <option key={s.id} value={s.id}>{s.vendor} - {s.name} ({s.remaining_weight}g)</option>)}
                        </select>
                      </div>
                    )}
                  </div>
                );
              })}
            </div>

            {/* Sync to U1 button */}
            <div style={{ background: "rgba(255,255,255,0.033)", border: "1px solid rgba(255,255,255,0.07)", borderRadius: "12px", padding: "20px", display: "flex", alignItems: "center", justifyContent: "space-between" }}>
              <div>
                <div style={{ fontSize: "14px", fontWeight: "600", color: "#F0F0F0", marginBottom: "4px" }}>Sync to Snapmaker U1</div>
                <div style={{ fontSize: "12px", color: "rgba(255,255,255,0.4)" }}>
                  Push loaded toolhead filament state to U1 via Moonraker /printer/filament_detect/set
                </div>
              </div>
              <button style={{
                padding: "12px 24px", borderRadius: "8px", fontSize: "13px", fontWeight: "600",
                background: "#00C9FF18", border: "1px solid #00C9FF55", color: "#00C9FF",
                cursor: "pointer", display: "flex", alignItems: "center", gap: "8px"
              }}>
                <IconSync /> PUSH TO U1
              </button>
            </div>
          </div>
        )}

        {/* -- ACTIVITY LOG TAB -- */}
        {tab === "log" && (
          <div style={{ animation: "slideIn 0.3s ease" }}>
            <div style={{ background: "rgba(255,255,255,0.033)", border: "1px solid rgba(255,255,255,0.07)", borderRadius: "12px", overflow: "hidden" }}>
              <table style={{ width: "100%", borderCollapse: "collapse" }}>
                <thead>
                  <tr style={{ borderBottom: "1px solid rgba(255,255,255,0.07)" }}>
                    {["TIMESTAMP", "SPOOL", "PREV WEIGHT", "NEW WEIGHT", "DELTA", "ACTION"].map(h => (
                      <th key={h} style={{ padding: "12px 16px", textAlign: "left", fontSize: "10px", color: "rgba(255,255,255,0.35)", letterSpacing: "0.1em", fontWeight: "600" }}>{h}</th>
                    ))}
                  </tr>
                </thead>
                <tbody>
                  {log.map((entry, i) => (
                    <tr key={entry.id} style={{ borderBottom: "1px solid rgba(255,255,255,0.04)", background: i % 2 === 0 ? "transparent" : "rgba(255,255,255,0.015)" }}>
                      <td style={{ padding: "12px 16px", fontSize: "12px", color: "rgba(255,255,255,0.4)", fontFamily: "monospace" }}>
                        {new Date(entry.ts).toLocaleString()}
                      </td>
                      <td style={{ padding: "12px 16px" }}>
                        <div style={{ fontSize: "13px", color: "#F0F0F0" }}>{entry.name}</div>
                        <div style={{ fontSize: "11px", color: "rgba(255,255,255,0.35)" }}>{entry.vendor}</div>
                      </td>
                      <td style={{ padding: "12px 16px", fontSize: "13px", fontFamily: "monospace", color: "rgba(255,255,255,0.5)" }}>{entry.old_weight}g</td>
                      <td style={{ padding: "12px 16px", fontSize: "13px", fontFamily: "monospace", color: "#E0E0E0" }}>{entry.new_weight}g</td>
                      <td style={{ padding: "12px 16px", fontSize: "13px", fontFamily: "monospace", color: entry.delta < 0 ? "#FF9500" : "#7FFF00" }}>
                        {entry.delta > 0 ? "+" : ""}{entry.delta}g
                      </td>
                      <td style={{ padding: "12px 16px" }}>
                        <span style={{
                          fontSize: "11px", padding: "3px 8px", borderRadius: "4px", fontWeight: "600",
                          background: "#00C9FF18", color: "#00C9FF", border: "1px solid #00C9FF33"
                        }}>{entry.action.toUpperCase()}</span>
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
              {log.length === 0 && (
                <div style={{ textAlign: "center", padding: "48px", color: "rgba(255,255,255,0.25)", fontSize: "14px" }}>No activity logged yet</div>
              )}
            </div>
          </div>
        )}

        {tab === "log" && u1Changes.length > 0 && (
          <div style={{ marginTop: "16px", background: "rgba(255,255,255,0.033)", border: "1px solid rgba(255,255,255,0.07)", borderRadius: "12px", padding: "16px 20px" }}>
            <div style={{ fontSize: "11px", color: "rgba(255,255,255,0.4)", letterSpacing: "0.1em", marginBottom: "12px" }}>LAST U1 SYNC CHANGES</div>
            {u1Changes.map((c, i) => (
              <div key={i} style={{ display: "flex", alignItems: "center", gap: "12px", padding: "7px 0", borderBottom: i < u1Changes.length - 1 ? "1px solid rgba(255,255,255,0.04)" : "none" }}>
                <span style={{
                  fontSize: "10px", padding: "2px 7px", borderRadius: "4px", fontWeight: "700",
                  background: c.action === "loaded" ? "#7FFF0018" : "#FF3B6B18",
                  color: c.action === "loaded" ? "#7FFF00" : "#FF3B6B",
                  border: `1px solid ${c.action === "loaded" ? "#7FFF0033" : "#FF3B6B33"}`
                }}>{c.action.toUpperCase()}</span>
                <span style={{ fontSize: "12px", color: "rgba(255,255,255,0.6)", fontFamily: "monospace" }}>
                  T{(c.channel ?? 0) + 1} - spool #{c.spool_id}
                </span>
              </div>
            ))}
          </div>
        )}
      </main>

      {/* -- MODAL -- */}
      {modalOpen && (
        <SpoolModal
          spool={modalSpool}
          onClose={() => setModalOpen(false)}
          onSave={(form) => {
            if (modalSpool) {
              setSpools(prev => prev.map(s => s.id === form.id ? form : s));
            } else {
              setSpools(prev => [...prev, { ...form, id: Date.now() }]);
            }
            setModalOpen(false);
          }}
        />
      )}

      {unknownTag && (
        <QuickRegisterModal
          uid={unknownTag.uid}
          weightG={unknownTag.weight_g}
          onClose={() => setUnknownTag(null)}
          onSave={(form) => {
            setSpools(prev => [...prev, { ...form, id: Date.now() }]);
            setUnknownTag(null);
          }}
        />
      )}
    </div>
  );
}
