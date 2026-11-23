function T = vbus_noise(dataDir, outDir, figDir)
%VBUS_NOISE  Ripple, noise and spectra of the 2026-10-10 VBUS captures.
%   T = VBUS_NOISE(dataDir, outDir, figDir) reads the DS1104Z captures in
%   dataDir (<op>_hf.mat, <op>_hfw.mat, <op>_lf.mat: raw 8-bit codes plus
%   scaling), writes vbus_noise_matlab.csv and vbus_tones_matlab.csv to
%   outDir and vbus_spectra_matlab.png to figDir (default outDir).  Needs
%   Signal Processing Toolbox.
%
%   Volts: v = (double(code) - yorig - yref) * yinc.  Methods (same as
%   analyze.py, so the two can be compared number for number):
%     DC          mean of the 600 ms record.
%     Ripple      Vpp and Vrms of the 9.6 ms record with the 20 MHz limit,
%                 and of the 9.6 ms full-bandwidth record.
%     Sync ripple p-p of the 200 kHz-synchronous waveform: the 20 MHz record
%                 through a 2 MHz low-pass (4th order, zero phase), averaged
%                 cycle by cycle over each quarter of the record; mean of the
%                 four quarters.
%     LF ripple   600 ms record, 8 Hz 4th-order Butterworth high-pass then a
%                 20 kHz low-pass, both zero phase, 50 ms edges dropped.
%     VLF swing   peak-to-peak of the 20 ms moving mean of the 600 ms record.
%     PSD         pwelch, periodic Hann, 50 % overlap; 2^18 points on the
%                 high-passed 600 ms record (used below 10 kHz), 2^17 on the
%                 9.6 ms records (used from 10 kHz).  The 2 MSa/s record's
%                 floor is aliased above ~30 kHz (HRES decimation), hence the
%                 split.
%     Band rms    sqrt of the trapezoidal PSD integral over [lo, hi).
%     Tones       periodic flat-top window over the whole record, V rms per
%                 line; local maxima >= 12 dB over the band's median, the 8
%                 largest.

if nargin < 1, dataDir = fileparts(mfilename('fullpath')); end
if nargin < 2, outDir = dataDir; end
if nargin < 3, figDir = outDir; end
ops = {'v05_p0','v09_p0','v15_p0','v15_p500','v15_p2000','v15_p3000', ...
       'v20_p0','v20_p500','v20_p2000','v20_p3000'};
fsplit = 10e3;
R = [];
tonesOut = [];
spec = struct();
for k = 1:numel(ops)
    lf = ld(fullfile(dataDir, [ops{k} '_lf.mat']));
    hf = ld(fullfile(dataDir, [ops{k} '_hf.mat']));
    hw = ld(fullfile(dataDir, [ops{k} '_hfw.mat']));
    r = struct();
    r.label = string(ops{k});
    r.contract_V = lf.contract_v;
    r.load_target_W = lf.load_target_w;
    r.vbus_dc_V = mean(lf.v);
    ac = hf.v - mean(hf.v);
    r.vpp_20MHz_mV = 1e3 * (max(ac) - min(ac));
    r.vrms_20MHz_mV = 1e3 * std(ac, 1);
    acw = hw.v - mean(hw.v);
    r.vpp_full_mV = 1e3 * (max(acw) - min(acw));
    r.vrms_full_mV = 1e3 * std(acw, 1);
    [z, p, g] = butter(4, 2e6 / (hf.fs / 2), 'low');
    [sos, gs] = zp2sos(z, p, g);
    xr = filtfilt(sos, gs, ac);
    n = round(hf.fs / 200e3);
    q = floor(floor(numel(xr) / n) / 4);
    pp = zeros(4, 1);
    for b = 1:4
        cyc = mean(reshape(xr((b - 1) * q * n + (1:q * n)), n, q), 2);
        pp(b) = max(cyc) - min(cyc);
    end
    r.ripple_sync_pp_mV = 1e3 * mean(pp);
    % 600 ms record
    aclf = lf.v - mean(lf.v);
    [z, p, g] = butter(4, 8 / (lf.fs / 2), 'high');
    [sos, gs] = zp2sos(z, p, g);
    hp = filtfilt(sos, gs, aclf);
    [z, p, g] = butter(4, 20e3 / (lf.fs / 2), 'low');
    [sos, gs] = zp2sos(z, p, g);
    bp = filtfilt(sos, gs, hp);
    e = round(0.05 * lf.fs);
    seg = hp(e + 1:end - e);
    bps = bp(e + 1:end - e);
    r.pp_8Hz_20kHz_mV = 1e3 * (max(bps) - min(bps));
    r.rms_8Hz_20kHz_mV = 1e3 * std(bps, 1);
    k20 = round(20e-3 * lf.fs);
    ma = conv(lf.v, ones(k20, 1) / k20, 'valid');
    r.vlf_swing_mV = 1e3 * (max(ma) - min(ma));
    % PSDs
    [pl, fl] = pwelch(seg - mean(seg), hann(2^18, 'periodic'), 2^17, 2^18, lf.fs);
    [ph, fh] = pwelch(ac, hann(2^17, 'periodic'), 2^16, 2^17, hf.fs);
    [pw, fw] = pwelch(acw, hann(2^17, 'periodic'), 2^16, 2^17, hw.fs);
    r.band_10Hz_1kHz_mV = 1e3 * sqrt(bandint(pl, fl, 10, 1e3));
    r.band_1kHz_10kHz_mV = 1e3 * sqrt(bandint(pl, fl, 1e3, 10e3));
    r.band_10kHz_100kHz_mV = 1e3 * sqrt(bandint(ph, fh, 10e3, 100e3));
    r.band_100kHz_1MHz_mV = 1e3 * sqrt(bandint(ph, fh, 100e3, 1e6));
    r.band_1MHz_20MHz_mV = 1e3 * sqrt(bandint(ph, fh, 1e6, 20e6));
    r.band_20MHz_62p5MHz_mV = 1e3 * sqrt(bandint(pw, fw, 20e6, 62.5e6));
    R = [R; r]; %#ok<AGROW>
    % tones
    tl = tones(seg - mean(seg), lf.fs, 10, fsplit);
    th = tones(ac, hf.fs, fsplit, 20e6);
    tt = [tl; th];
    if ~isempty(tt)
        tt.label = repmat(string(ops{k}), height(tt), 1);
        tonesOut = [tonesOut; tt]; %#ok<AGROW>
    end
    spec.(ops{k}) = struct('fl', fl, 'pl', pl, 'fh', fh, 'ph', ph);
end
T = struct2table(R);
writetable(T, fullfile(outDir, 'vbus_noise_matlab.csv'));
if ~isempty(tonesOut)
    writetable(tonesOut(:, {'label', 'f_Hz', 'mV_rms', 'dBuV'}), fullfile(outDir, 'vbus_tones_matlab.csv'));
end

% stitched amplitude spectral density per contract voltage
f = figure('Visible', 'off', 'Position', [0 0 1400 900]);
volts = [5 9 15 20];
for s = 1:4
    subplot(2, 2, s);
    for k = 1:numel(ops)
        if R(k).contract_V ~= volts(s), continue; end
        q = spec.(ops{k});
        ml = q.fl >= 10 & q.fl < fsplit;
        mh = q.fh >= fsplit & q.fh < 20e6;
        loglog([q.fl(ml); q.fh(mh)], sqrt([q.pl(ml); q.ph(mh)]), 'DisplayName', sprintf('%g W', R(k).load_target_W));
        hold on;
    end
    xline(fsplit, ':');
    grid on; xlim([10 20e6]);
    title(sprintf('VBUS %g V contract (MATLAB pwelch)', volts(s)));
    xlabel('frequency [Hz]'); ylabel('V/\surdHz');
    legend('Location', 'southwest');
end
exportgraphics(f, fullfile(figDir, 'vbus_spectra_matlab.png'), 'Resolution', 120);
close(f);
fprintf('VBUS_NOISE_DONE %d operating points\n', height(T));
end

function d = ld(file)
d = load(file);
d.v = (double(d.code) - d.yorig - d.yref) * d.yinc;
end

function p = bandint(pxx, f, lo, hi)
m = f >= lo & f < hi;
p = trapz(f(m), pxx(m));
end

function t = tones(x, fs, fmin, fmax)
n = numel(x);
w = flattopwin(n, 'periodic');
X = fft(x(:) .* w);
a = abs(X(1:floor(n / 2) + 1)) * 2 / sum(w) / sqrt(2);
fr = (0:floor(n / 2))' * fs / n;
m = fr >= fmin & fr < fmax;
fi = fr(m); ai = a(m);
floorv = median(ai);
dist = max(1, floor(n / fs * max(fmin, 5)));
[pk, loc] = findpeaks(ai, 'MinPeakDistance', dist);
keep = pk > floorv * 10^(12 / 20);
pk = pk(keep); loc = loc(keep);
[~, ord] = sort(pk, 'descend');
ord = ord(1:min(8, numel(ord)));
loc = sort(loc(ord));
t = table(fi(loc), 1e3 * ai(loc), 20 * log10(ai(loc) / 1e-6), 'VariableNames', {'f_Hz', 'mV_rms', 'dBuV'});
end
