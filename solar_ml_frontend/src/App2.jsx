import { useState, useEffect, useRef, useCallback } from "react";

const PREDICT_URL = "http://127.0.0.1:8000/predict";
const THEME_KEY = "solarMonitorTheme";

function formatPower(w) {
  return `${Math.round(w).toLocaleString()} W`;
}

function formatPR(pr) {
  return pr === null || pr === undefined ? "— night" : pr.toFixed(3);
}

function formatHour(isoTimestamp) {
  return new Date(isoTimestamp).toLocaleTimeString([], {
    hour: "numeric",
    minute: "2-digit",
  });
}

function formatRelative(date) {
  if (!date) return "never";
  const seconds = Math.floor((Date.now() - date.getTime()) / 1000);
  if (seconds < 45) return "just now";
  const minutes = Math.floor(seconds / 60);
  if (minutes < 60) return `${minutes}m ago`;
  const hours = Math.floor(minutes / 60);
  return `${hours}h ago`;
}

function msUntilNextHour() {
  const now = new Date();
  const next = new Date(now);
  next.setMinutes(0, 0, 0);
  next.setHours(now.getHours() + 1);
  return next.getTime() - now.getTime();
}

function getInitialTheme() {
  const stored = window.localStorage.getItem(THEME_KEY);
  if (stored === "light" || stored === "dark") return stored;
  return window.matchMedia("(prefers-color-scheme: light)").matches
    ? "light"
    : "dark";
}

export default function PredictionDashboard() {
  const [current, setCurrent] = useState(null);
  const [next, setNext] = useState(null);
  const [status, setStatus] = useState("loading"); // loading | ready | refreshing | error
  const [lastFetchedAt, setLastFetchedAt] = useState(null);
  const [justUpdated, setJustUpdated] = useState(false);
  const [theme, setTheme] = useState(getInitialTheme);
  const [, forceTick] = useState(0);
  const timerRef = useRef(null);

  const fetchPredictions = useCallback(async () => {
    setStatus((s) => (s === "loading" ? "loading" : "refreshing"));
    try {
      const res = await fetch(PREDICT_URL);
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const body = await res.json();
      const rows = [...body.predictions].sort(
        (a, b) => new Date(a.timestamp) - new Date(b.timestamp)
      );
      setCurrent(rows[0] ?? null);
      setNext(rows[1] ?? null);
      setLastFetchedAt(new Date());
      setStatus("ready");
      setJustUpdated(true);
      setTimeout(() => setJustUpdated(false), 900);
    } catch (err) {
      setStatus("error");
    }
  }, []);

  useEffect(() => {
    fetchPredictions();
  }, [fetchPredictions]);

  useEffect(() => {
    function schedule() {
      timerRef.current = setTimeout(() => {
        fetchPredictions();
        schedule();
      }, msUntilNextHour());
    }
    schedule();
    return () => clearTimeout(timerRef.current);
  }, [fetchPredictions]);

  useEffect(() => {
    const id = setInterval(() => forceTick((n) => n + 1), 30000);
    return () => clearInterval(id);
  }, []);

  useEffect(() => {
    window.localStorage.setItem(THEME_KEY, theme);
  }, [theme]);

  const isStale =
    current && new Date(current.timestamp).getHours() !== new Date().getHours();

  return (
    <div className="panel-root" data-theme={theme}>
      <style>{`
        .panel-root[data-theme="dark"] {
          --bg: #14140F;
          --panel: #1C1C15;
          --panel-border: #2C2C22;
          --text: #F2EFE6;
          --text-muted: #8F9184;
          --accent: #E8A33D;
          --accent-secondary: #5A8CA0;
          --danger: #C1533A;
          --ok: #7FAE6A;
          --banner-bg: rgba(193, 83, 58, 0.12);
          --banner-border: rgba(193, 83, 58, 0.4);
          --banner-text: #E5A192;
          --btn-bg: #1C1C15;
          --btn-active-bg: #22221A;
        }
        .panel-root[data-theme="light"] {
          --bg: #F3EFE4;
          --panel: #FFFFFF;
          --panel-border: #DDD6C4;
          --text: #2B2A22;
          --text-muted: #7D7A68;
          --accent: #B9761E;
          --accent-secondary: #35657A;
          --danger: #A63B27;
          --ok: #4C7A3E;
          --banner-bg: rgba(166, 59, 39, 0.08);
          --banner-border: rgba(166, 59, 39, 0.35);
          --banner-text: #8A3524;
          --btn-bg: #FFFFFF;
          --btn-active-bg: #F0EBDD;
        }
        .panel-root {
          min-height: 100vh;
          background: var(--bg);
          display: flex;
          align-items: center;
          justify-content: center;
          font-family: ui-sans-serif, system-ui, -apple-system, "Segoe UI", sans-serif;
          padding: 24px;
          transition: background 0.15s ease;
        }
        .card { width: 100%; max-width: 420px; color: var(--text); }
        .header-row {
          display: flex;
          align-items: center;
          justify-content: space-between;
          margin-bottom: 6px;
        }
        .title { font-size: 15px; font-weight: 600; letter-spacing: 0.01em; }
        .status-row {
          display: flex;
          align-items: center;
          gap: 8px;
          font-size: 12px;
          color: var(--text-muted);
          margin-bottom: 24px;
        }
        .dot { width: 7px; height: 7px; border-radius: 2px; background: var(--ok); }
        .dot.stale, .dot.error { background: var(--danger); }
        .theme-toggle {
          display: flex;
          align-items: center;
          gap: 8px;
          background: transparent;
          border: none;
          cursor: pointer;
          color: var(--text-muted);
          font-size: 12px;
          padding: 4px;
        }
        .theme-toggle:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
        .toggle-track {
          width: 32px;
          height: 17px;
          border-radius: 9px;
          background: var(--panel-border);
          position: relative;
          flex-shrink: 0;
        }
        .toggle-knob {
          position: absolute;
          top: 2px;
          left: 2px;
          width: 13px;
          height: 13px;
          border-radius: 50%;
          background: var(--accent);
          transition: transform 0.2s ease;
          transform: translateX(0);
        }
        [data-theme="light"] .toggle-knob { transform: translateX(15px); }
        @media (prefers-reduced-motion: reduce) {
          .toggle-knob { transition: none; }
        }
        .primary { text-align: center; padding: 8px 0 24px; }
        .primary-label { font-size: 13px; color: var(--text-muted); margin-bottom: 6px; }
        .primary-power {
          font-family: ui-monospace, "IBM Plex Mono", Menlo, Consolas, monospace;
          font-variant-numeric: tabular-nums;
          font-size: 56px;
          font-weight: 600;
          color: var(--accent);
          line-height: 1;
          transition: opacity 0.2s ease;
        }
        .primary-power.pulse { animation: glow 0.9s ease; }
        @media (prefers-reduced-motion: reduce) { .primary-power.pulse { animation: none; } }
        @keyframes glow { 0% { opacity: 0.35; } 100% { opacity: 1; } }
        .primary-pr {
          margin-top: 10px;
          font-family: ui-monospace, "IBM Plex Mono", Menlo, Consolas, monospace;
          font-size: 20px;
          color: var(--text);
        }
        .primary-pr-label { font-size: 12px; color: var(--text-muted); margin-left: 8px; }
        .next {
          border-top: 1px solid var(--panel-border);
          padding: 16px 4px;
          display: flex;
          align-items: center;
          justify-content: space-between;
        }
        .next-label { font-size: 13px; color: var(--accent-secondary); }
        .next-values {
          font-family: ui-monospace, "IBM Plex Mono", Menlo, Consolas, monospace;
          font-size: 15px;
          color: var(--text);
          display: flex;
          gap: 14px;
        }
        .next-values span.pr { color: var(--text-muted); }
        .banner {
          margin-top: 16px;
          padding: 10px 12px;
          background: var(--banner-bg);
          border: 1px solid var(--banner-border);
          color: var(--banner-text);
          font-size: 13px;
          border-radius: 3px;
        }
        .refresh-btn {
          margin-top: 20px;
          width: 100%;
          padding: 14px;
          background: var(--btn-bg);
          border: 1px solid var(--panel-border);
          color: var(--text);
          font-size: 14px;
          font-weight: 500;
          border-radius: 3px;
          cursor: pointer;
        }
        .refresh-btn:active { background: var(--btn-active-bg); }
        .refresh-btn:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
      `}</style>

      <div className="card">
        <div className="header-row">
          <span className="title">Solar Power Forecast monitor</span>
          <button
            className="theme-toggle"
            onClick={() => setTheme((t) => (t === "dark" ? "light" : "dark"))}
            aria-pressed={theme === "light"}
            aria-label="Switch color theme"
          >
            <span className="toggle-track">
              <span className="toggle-knob" />
            </span>
            {theme === "dark" ? "Dark" : "Light"}
          </button>
        </div>
        <div className="status-row">
          <span className={`dot ${status === "error" ? "error" : isStale ? "stale" : ""}`} />
          {status === "loading" ? "Loading prediction…" : `Updated ${formatRelative(lastFetchedAt)}`}
        </div>

        {status === "loading" && !current ? (
          <div className="primary">
            <div className="primary-label">Reading the forecast</div>
          </div>
        ) : (
          <>
            <div className="primary">
              <div className="primary-label">
                Power right now · {current ? formatHour(current.timestamp) : "—"}
              </div>
              <div className={`primary-power ${justUpdated ? "pulse" : ""}`}>
                {current ? formatPower(current.predicted_power_w) : "—"}
              </div>
              <div className="primary-pr">
                {current ? formatPR(current.predicted_pr) : "—"}
                <span className="primary-pr-label">performance ratio</span>
              </div>
            </div>

            {next && (
              <div className="next">
                <span className="next-label">Next hour · {formatHour(next.timestamp)}</span>
                <span className="next-values">
                  <span>{formatPower(next.predicted_power_w)}</span>
                  <span className="pr">{formatPR(next.predicted_pr)}</span>
                </span>
              </div>
            )}
          </>
        )}

        {status === "error" && (
          <div className="banner">Couldn't reach the predictor. Showing last known values.</div>
        )}
        {isStale && status !== "error" && (
          <div className="banner">This reading may be out of date.</div>
        )}

        <button className="refresh-btn" onClick={() => fetchPredictions()}>
          Refresh now
        </button>
      </div>
    </div>
  );
}
