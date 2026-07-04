const fmt = (value, digits = 3) => Number.isFinite(value) ? value.toFixed(digits) : '—';
const forceError = (capacity, accuracyPct) => Math.abs(capacity * accuracyPct / 100);
const rss = values => Math.sqrt(values.reduce((sum, value) => sum + value * value, 0));
const sampleRange = (center, tolerance, count = 41) => tolerance <= 0 ? [center] : Array.from({ length: count }, (_, i) => center - tolerance + (2 * tolerance * i) / (count - 1));
const unique = items => [...new Set(items)];

const one = { x1: 0, x2: 1000, positionTolerance: 0.5, capacity: 100, accuracyPct: 0.05, weight: 60, cg: 500, cgTolerance: 20, maxError: 2 };
const two = { x1: 0, y1: 0, x2: 1000, y2: 0, x3: 500, y3: 800, positionTolerance: 0.5, capacity: 100, accuracyPct: 0.05, weight: 60, cgx: 500, cgy: 300, cgRadialTolerance: 20, maxError: 3 };

function compatibility(error, maxError) {
  if (!Number.isFinite(error) || maxError <= 0) return { label: 'Maksimum hata girilmedi', status: 'warn' };
  if (error <= maxError) return { label: 'Uygun', status: 'good' };
  if (error <= maxError * 1.15) return { label: 'Sınırda', status: 'warn' };
  return { label: 'Uygun değil', status: 'bad' };
}

function calculate1D(values) {
  const span = values.x2 - values.x1;
  const errorKg = forceError(values.capacity, values.accuracyPct);
  const warnings = [];
  if (Math.abs(span) < 1e-9) warnings.push('LC1 ve LC2 X konumları aynı olamaz.');
  if (values.weight <= 0) warnings.push('Toplam ürün ağırlığı 0 kg üzerinde olmalı.');
  if (values.capacity <= 0) warnings.push('Loadcell kapasitesi 0 kg üzerinde olmalı.');
  if (values.cg - values.cgTolerance < Math.min(values.x1, values.x2) || values.cg + values.cgTolerance > Math.max(values.x1, values.x2)) warnings.push('Beklenen CG aralığı loadcell destek aralığının dışına taşıyor.');
  if (warnings.some(warning => warning.includes('olamaz') || warning.includes('olmalı'))) return { warnings, errorKg, nominalF1: NaN, nominalF2: NaN, worst: NaN, rssValue: NaN, compliance: compatibility(NaN, values.maxError), worstAt: NaN };

  let worst = 0;
  let worstAt = values.cg;
  let rssValue = 0;
  for (const actualCg of sampleRange(values.cg, values.cgTolerance)) {
    const nominalF2 = values.weight * (actualCg - values.x1) / span;
    const nominalF1 = values.weight - nominalF2;
    if (nominalF1 < 0 || nominalF2 < 0) warnings.push(`CG ${fmt(actualCg)} mm noktasında negatif reaksiyon oluşuyor.`);
    const cases = [
      { f1: nominalF1 + errorKg, f2: nominalF2 - errorKg, p1: values.x1 + values.positionTolerance, p2: values.x2 - values.positionTolerance },
      { f1: nominalF1 - errorKg, f2: nominalF2 + errorKg, p1: values.x1 - values.positionTolerance, p2: values.x2 + values.positionTolerance },
      { f1: nominalF1 + errorKg, f2: nominalF2 + errorKg, p1: values.x1 + values.positionTolerance, p2: values.x2 + values.positionTolerance },
      { f1: nominalF1 - errorKg, f2: nominalF2 - errorKg, p1: values.x1 - values.positionTolerance, p2: values.x2 - values.positionTolerance },
    ];
    for (const item of cases) {
      const measured = (item.f1 * item.p1 + item.f2 * item.p2) / (item.f1 + item.f2);
      const cgError = Math.abs(measured - actualCg);
      if (cgError > worst) {
        worst = cgError;
        worstAt = actualCg;
      }
    }
    const forcePart = (Math.abs(values.x1 - actualCg) + Math.abs(values.x2 - actualCg)) * errorKg / values.weight;
    rssValue = Math.max(rssValue, rss([forcePart, values.positionTolerance]));
  }
  const nominalF2 = values.weight * (values.cg - values.x1) / span;
  return { warnings: unique(warnings), errorKg, nominalF1: values.weight - nominalF2, nominalF2, worst, rssValue, compliance: compatibility(worst, values.maxError), worstAt };
}

function solve3(a, b) {
  const m = a.map((row, i) => [...row, b[i]]);
  for (let col = 0; col < 3; col++) {
    let pivot = col;
    for (let row = col + 1; row < 3; row++) if (Math.abs(m[row][col]) > Math.abs(m[pivot][col])) pivot = row;
    if (Math.abs(m[pivot][col]) < 1e-9) return null;
    [m[col], m[pivot]] = [m[pivot], m[col]];
    const div = m[col][col];
    for (let j = col; j < 4; j++) m[col][j] /= div;
    for (let row = 0; row < 3; row++) if (row !== col) {
      const factor = m[row][col];
      for (let j = col; j < 4; j++) m[row][j] -= factor * m[col][j];
    }
  }
  return [m[0][3], m[1][3], m[2][3]];
}

function triangleArea(a, b, c) {
  return Math.abs((a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y)) / 2);
}

function pointInTriangle(p, a, b, c) {
  const area = triangleArea(a, b, c);
  const total = triangleArea(p, b, c) + triangleArea(a, p, c) + triangleArea(a, b, p);
  return Math.abs(total - area) <= 1e-6;
}

function calculate2D(values) {
  const points = [{ x: values.x1, y: values.y1 }, { x: values.x2, y: values.y2 }, { x: values.x3, y: values.y3 }];
  const errorKg = forceError(values.capacity, values.accuracyPct);
  const warnings = [];
  if (triangleArea(points[0], points[1], points[2]) < 1e-9) warnings.push('3 loadcell aynı doğru üzerinde olamaz.');
  if (values.weight <= 0) warnings.push('Toplam ürün ağırlığı 0 kg üzerinde olmalı.');
  if (values.capacity <= 0) warnings.push('Loadcell kapasitesi 0 kg üzerinde olmalı.');
  if (!pointInTriangle({ x: values.cgx, y: values.cgy }, points[0], points[1], points[2])) warnings.push('Beklenen CG destek üçgeninin dışında.');
  if (warnings.some(warning => warning.includes('olamaz') || warning.includes('olmalı'))) return { warnings, errorKg, nominal: [NaN, NaN, NaN], worstX: NaN, worstY: NaN, worstRadial: NaN, rssX: NaN, rssY: NaN, rssRadial: NaN, compliance: compatibility(NaN, values.maxError), worstAt: { x: NaN, y: NaN } };

  let worstX = 0;
  let worstY = 0;
  let worstRadial = 0;
  let rssX = 0;
  let rssY = 0;
  let worstAt = { x: values.cgx, y: values.cgy };
  const samples = [{ x: values.cgx, y: values.cgy }];
  if (values.cgRadialTolerance > 0) {
    for (let rStep = 1; rStep <= 5; rStep++) {
      const radius = values.cgRadialTolerance * rStep / 5;
      for (let i = 0; i < 24; i++) samples.push({ x: values.cgx + radius * Math.cos(2 * Math.PI * i / 24), y: values.cgy + radius * Math.sin(2 * Math.PI * i / 24) });
    }
  }

  for (const cgPoint of samples) {
    if (!pointInTriangle(cgPoint, points[0], points[1], points[2])) warnings.push('Beklenen CG radyal tolerans alanı destek üçgeninin dışına taşıyor.');
    const nominal = solve3([[1, 1, 1], [points[0].x, points[1].x, points[2].x], [points[0].y, points[1].y, points[2].y]], [values.weight, values.weight * cgPoint.x, values.weight * cgPoint.y]);
    if (!nominal) continue;
    if (nominal.some(value => value < 0)) warnings.push('Bazı CG noktalarında negatif loadcell reaksiyonu oluşuyor.');
    for (const s1 of [-1, 1]) for (const s2 of [-1, 1]) for (const s3 of [-1, 1]) {
      const forces = [nominal[0] + s1 * errorKg, nominal[1] + s2 * errorKg, nominal[2] + s3 * errorKg];
      const total = forces[0] + forces[1] + forces[2];
      for (const px of [-1, 1]) for (const py of [-1, 1]) {
        const shifted = points.map(point => ({ x: point.x + px * values.positionTolerance, y: point.y + py * values.positionTolerance }));
        const measuredX = forces.reduce((sum, value, i) => sum + value * shifted[i].x, 0) / total;
        const measuredY = forces.reduce((sum, value, i) => sum + value * shifted[i].y, 0) / total;
        const ex = Math.abs(measuredX - cgPoint.x);
        const ey = Math.abs(measuredY - cgPoint.y);
        const er = Math.hypot(ex, ey);
        worstX = Math.max(worstX, ex);
        worstY = Math.max(worstY, ey);
        if (er > worstRadial) {
          worstRadial = er;
          worstAt = cgPoint;
        }
      }
    }
    const forcePartX = points.reduce((sum, point) => sum + Math.abs(point.x - cgPoint.x), 0) * errorKg / values.weight;
    const forcePartY = points.reduce((sum, point) => sum + Math.abs(point.y - cgPoint.y), 0) * errorKg / values.weight;
    rssX = Math.max(rssX, rss([forcePartX, values.positionTolerance]));
    rssY = Math.max(rssY, rss([forcePartY, values.positionTolerance]));
  }
  const nominal = solve3([[1, 1, 1], [points[0].x, points[1].x, points[2].x], [points[0].y, points[1].y, points[2].y]], [values.weight, values.weight * values.cgx, values.weight * values.cgy]) || [NaN, NaN, NaN];
  return { warnings: unique(warnings), errorKg, nominal, worstX, worstY, worstRadial, rssX, rssY, rssRadial: Math.hypot(rssX, rssY), compliance: compatibility(worstRadial, values.maxError), worstAt };
}

function input(label, state, key, suffix, render) {
  return `<label class="field"><span>${label}</span><div><input type="number" step="any" value="${state[key]}" data-key="${key}" data-render="${render}"><em>${suffix}</em></div></label>`;
}

function resultCard(title, status, lines, warnings) {
  return `<aside class="result ${status}"><h2>${title}</h2>${lines.map(line => `<p>${line}</p>`).join('')}${warnings.length ? `<div class="warnings"><strong>Uyarılar</strong>${warnings.map(warning => `<span>${warning}</span>`).join('')}</div>` : ''}<small>Not: Worst-case, loadcell kuvvet hatası ve konum toleransının ters işaretli uç senaryolarını tarar; RSS bağımsız belirsizlik varsayımıyla hesaplanır.</small></aside>`;
}

function render1D() {
  const result = calculate1D(one);
  document.querySelector('#onePanel').innerHTML = `<div class="grid">
    ${input('LC1 X konumu', one, 'x1', 'mm', '1d')}${input('LC2 X konumu', one, 'x2', 'mm', '1d')}${input('Konum toleransı', one, 'positionTolerance', '± mm', '1d')}${input('Loadcell kapasitesi', one, 'capacity', 'kg', '1d')}${input('Loadcell doğruluğu', one, 'accuracyPct', '%FS', '1d')}${input('Toplam ürün ağırlığı', one, 'weight', 'kg', '1d')}${input('Beklenen CG X', one, 'cg', 'mm', '1d')}${input('Beklenen CG toleransı', one, 'cgTolerance', '± mm', '1d')}${input('Maks. CG hatası', one, 'maxError', 'mm', '1d')}
  </div>${resultCard('1D Sonuçları', result.compliance.status, [`LC kuvvet belirsizliği: ±${fmt(result.errorKg)} kg`, `Nominal LC1 / LC2: ${fmt(result.nominalF1)} kg / ${fmt(result.nominalF2)} kg`, `Worst-case CG hatası: ±${fmt(result.worst)} mm`, `RSS CG hatası: ±${fmt(result.rssValue)} mm`, `En kötü CG noktası: ${fmt(result.worstAt)} mm`, `Uyum: ${result.compliance.label}`], result.warnings)}`;
}

function render2D() {
  const result = calculate2D(two);
  document.querySelector('#twoPanel').innerHTML = `<div class="grid">
    ${input('LC1 X', two, 'x1', 'mm', '2d')}${input('LC1 Y', two, 'y1', 'mm', '2d')}${input('LC2 X', two, 'x2', 'mm', '2d')}${input('LC2 Y', two, 'y2', 'mm', '2d')}${input('LC3 X', two, 'x3', 'mm', '2d')}${input('LC3 Y', two, 'y3', 'mm', '2d')}${input('Konum toleransı', two, 'positionTolerance', '± mm', '2d')}${input('Loadcell kapasitesi', two, 'capacity', 'kg', '2d')}${input('Loadcell doğruluğu', two, 'accuracyPct', '%FS', '2d')}${input('Toplam ürün ağırlığı', two, 'weight', 'kg', '2d')}${input('Beklenen CG X', two, 'cgx', 'mm', '2d')}${input('Beklenen CG Y', two, 'cgy', 'mm', '2d')}${input('CG radyal toleransı', two, 'cgRadialTolerance', '± mm', '2d')}${input('Maks. radyal CG hatası', two, 'maxError', 'mm', '2d')}
  </div>${resultCard('2D Sonuçları', result.compliance.status, [`LC kuvvet belirsizliği: ±${fmt(result.errorKg)} kg`, `Nominal LC1 / LC2 / LC3: ${result.nominal.map(value => `${fmt(Math.abs(value) < 1e-9 ? 0 : value)} kg`).join(' / ')}`, `Worst-case X/Y/radyal: ±${fmt(result.worstX)} / ±${fmt(result.worstY)} / ±${fmt(result.worstRadial)} mm`, `RSS X/Y/radyal: ±${fmt(result.rssX)} / ±${fmt(result.rssY)} / ±${fmt(result.rssRadial)} mm`, `En kötü CG noktası: (${fmt(result.worstAt.x)}, ${fmt(result.worstAt.y)}) mm`, `Uyum: ${result.compliance.label}`], result.warnings)}`;
}

document.addEventListener('input', event => {
  const element = event.target;
  if (!element.matches('input[data-key]')) return;
  const target = element.dataset.render === '1d' ? one : two;
  target[element.dataset.key] = Number(element.value);
  element.dataset.render === '1d' ? render1D() : render2D();
});

document.querySelector('#tab1').addEventListener('click', () => {
  document.querySelector('#tab1').classList.add('active');
  document.querySelector('#tab2').classList.remove('active');
  document.querySelector('#onePanel').classList.remove('hidden');
  document.querySelector('#twoPanel').classList.add('hidden');
});
document.querySelector('#tab2').addEventListener('click', () => {
  document.querySelector('#tab2').classList.add('active');
  document.querySelector('#tab1').classList.remove('active');
  document.querySelector('#twoPanel').classList.remove('hidden');
  document.querySelector('#onePanel').classList.add('hidden');
});

render1D();
render2D();
