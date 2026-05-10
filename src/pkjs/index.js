var Clay = require('@rebble/clay');
var clayConfig = require('./config.json');
var clay = new Clay(clayConfig, null, { autoHandleEvents: false });

var messageQueue = [];
var sending = false;

function sendMsg(dict) {
    messageQueue.push(dict);
    processQueue();
}

function processQueue() {
    if (sending || messageQueue.length === 0) return;
    sending = true;
    var msg = messageQueue.shift();
    Pebble.sendAppMessage(msg,
        function () { sending = false; processQueue(); },
        function () { sending = false; processQueue(); }
    );
}

function getIcsUrl() {
    var raw = localStorage.getItem('ics_url');
    return raw || '';
}

function getTempUnit() {
    var raw = localStorage.getItem('temp_unit');
    return raw === '1' ? 1 : 0;
}

function fetchWeather() {
    var unit = getTempUnit() === 1 ? 'celsius' : 'fahrenheit';
    navigator.geolocation.getCurrentPosition(
        function (pos) {
            var url = 'https://api.open-meteo.com/v1/forecast' +
                '?latitude=' + pos.coords.latitude +
                '&longitude=' + pos.coords.longitude +
                '&daily=temperature_2m_max,weather_code' +
                '&current=temperature_2m,weather_code' +
                '&temperature_unit=' + unit +
                '&timezone=auto&forecast_days=7';
            var xhr = new XMLHttpRequest();
            xhr.onload = function () {
                try {
                    var data = JSON.parse(this.responseText);
                    var temps = data.daily.temperature_2m_max.map(function (t) {
                        return Math.round(t);
                    });
                    var codes = data.daily.weather_code.slice();
                    temps[0] = Math.round(data.current.temperature_2m);
                    codes[0] = data.current.weather_code;
                    temps = temps.join(',');
                    codes = codes.join(',');
                    sendMsg({
                        'FORECAST_TEMPS': temps,
                        'FORECAST_CODES': codes,
                        'TEMP_UNIT': getTempUnit()
                    });
                } catch (e) {
                    console.log('Weather parse error: ' + e);
                }
            };
            xhr.onerror = function () { console.log('Weather fetch failed'); };
            xhr.timeout = 30000;
            xhr.open('GET', url);
            xhr.send();
        },
        function (err) { console.log('Geo error: ' + err.message); },
        { timeout: 15000, maximumAge: 60000 }
    );
}

function parseICSDate(str) {
    return new Date(parseInt(str.substring(0, 4), 10),
        parseInt(str.substring(4, 6), 10) - 1,
        parseInt(str.substring(6, 8), 10));
}

function parseICSDateTime(str) {
    if (!str || str.length < 15) return null;
    var y = parseInt(str.substring(0, 4), 10);
    var m = parseInt(str.substring(4, 6), 10) - 1;
    var d = parseInt(str.substring(6, 8), 10);
    var h = parseInt(str.substring(9, 11), 10);
    var mn = parseInt(str.substring(11, 13), 10);
    var s = parseInt(str.substring(13, 15), 10);
    if (str.charAt(str.length - 1) === 'Z') {
        return new Date(Date.UTC(y, m, d, h, mn, s));
    }
    return new Date(y, m, d, h, mn, s);
}

function parseEvent(block) {
    var lines = block.split('\n');
    var ev = { summary: '', start: null, allDay: false, rrule: null, exdates: [] };
    for (var i = 0; i < lines.length; i++) {
        var line = lines[i].trim();
        if (line.indexOf('SUMMARY:') === 0) {
            ev.summary = line.substring(8).replace(/\\n/g, ' ').replace(/\\,/g, ',').replace(/\\\\/g, '\\');
        } else if (line.indexOf('DTSTART') === 0) {
            var val = line.split(':').pop();
            if (line.indexOf('VALUE=DATE') !== -1) {
                ev.allDay = true;
                ev.start = parseICSDate(val);
            } else {
                ev.start = parseICSDateTime(val);
                if (!ev.start) ev.start = parseICSDate(val);
            }
        } else if (line.indexOf('RRULE:') === 0) {
            ev.rrule = line.substring(6);
        } else if (line.indexOf('EXDATE') === 0) {
            var exd = parseICSDateTime(line.split(':').pop());
            if (exd) ev.exdates.push(exd);
        } else if (line.indexOf('STATUS:CANCELLED') !== -1) {
            return null;
        }
    }
    if (!ev.start || !ev.summary) return null;
    return ev;
}

function expandRecurrence(event, after) {
    var rrule = event.rrule;
    var freqMatch = rrule.match(/FREQ=(\w+)/);
    if (!freqMatch) return null;
    var freq = freqMatch[1];
    var interval = parseInt((rrule.match(/INTERVAL=(\d+)/) || [, '1'])[1], 10);
    var untilMatch = rrule.match(/UNTIL=([^;]+)/);
    var until = untilMatch ? (parseICSDateTime(untilMatch[1]) || parseICSDate(untilMatch[1])) : null;
    var countMatch = rrule.match(/COUNT=(\d+)/);
    var maxCount = countMatch ? parseInt(countMatch[1], 10) : 9999;
    var maxLookahead = new Date(after.getTime() + 30 * 86400000);
    if (until && until < after) return null;
    var candidate = new Date(event.start);
    var occ = 0;
    while (candidate <= maxLookahead && occ < maxCount) {
        occ++;
        if (until && candidate > until) return null;
        if (candidate > after) {
            var excluded = false;
            for (var j = 0; j < event.exdates.length; j++) {
                if (Math.abs(candidate - event.exdates[j]) < 86400000) { excluded = true; break; }
            }
            if (!excluded) return new Date(candidate);
        }
        switch (freq) {
            case 'DAILY': candidate = new Date(candidate.getTime() + interval * 86400000); break;
            case 'WEEKLY': candidate = new Date(candidate.getTime() + interval * 7 * 86400000); break;
            case 'MONTHLY': candidate = new Date(candidate); candidate.setMonth(candidate.getMonth() + interval); break;
            case 'YEARLY': candidate = new Date(candidate); candidate.setFullYear(candidate.getFullYear() + interval); break;
            default: return null;
        }
    }
    return null;
}

function findNextEvent(events) {
    var now = new Date();
    var candidates = [];
    for (var i = 0; i < events.length; i++) {
        var ev = events[i];
        if (ev.rrule) {
            var next = expandRecurrence(ev, now);
            if (next) candidates.push({ summary: ev.summary, start: next, allDay: ev.allDay });
        } else if (ev.start > now) {
            candidates.push(ev);
        }
    }
    candidates.sort(function (a, b) { return a.start - b.start; });
    return candidates.length > 0 ? candidates[0] : null;
}

function formatEventTime(date, allDay) {
    var now = new Date();
    var tomorrow = new Date(now);
    tomorrow.setDate(tomorrow.getDate() + 1);
    var dayStr;
    if (date.toDateString() === now.toDateString()) { dayStr = 'Today'; }
    else if (date.toDateString() === tomorrow.toDateString()) { dayStr = 'Tomorrow'; }
    else {
        var dn = ['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];
        var mn = ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];
        dayStr = dn[date.getDay()] + ', ' + mn[date.getMonth()] + ' ' + date.getDate();
    }
    if (allDay) return dayStr + ' (All day)';
    var h = date.getHours(), m = date.getMinutes();
    var ampm = h >= 12 ? 'PM' : 'AM';
    h = h % 12; if (h === 0) h = 12;
    return dayStr + ' ' + h + ':' + (m < 10 ? '0' : '') + m + ' ' + ampm;
}

function fetchCalendar() {
    var icsUrl = getIcsUrl();
    if (!icsUrl) {
        sendMsg({ 'CAL_TITLE': 'Set calendar in settings', 'CAL_TIME': '' });
        return;
    }
    var xhr = new XMLHttpRequest();
    xhr.onload = function () {
        try {
            var text = this.responseText.replace(/\r\n[\t ]/g, '').replace(/\r\n/g, '\n').replace(/\r/g, '\n');
            var events = [];
            var blocks = text.split('BEGIN:VEVENT');
            for (var i = 1; i < blocks.length; i++) {
                var ev = parseEvent(blocks[i].split('END:VEVENT')[0]);
                if (ev) events.push(ev);
            }
            var next = findNextEvent(events);
            if (next) {
                sendMsg({ 'CAL_TITLE': next.summary.substring(0, 63), 'CAL_TIME': formatEventTime(next.start, next.allDay) });
            } else {
                sendMsg({ 'CAL_TITLE': 'No upcoming events', 'CAL_TIME': '' });
            }
        } catch (e) {
            console.log('ICS parse error: ' + e);
            sendMsg({ 'CAL_TITLE': 'Calendar error', 'CAL_TIME': '' });
        }
    };
    xhr.onerror = function () { sendMsg({ 'CAL_TITLE': 'Calendar unavailable', 'CAL_TIME': '' }); };
    xhr.timeout = 30000;
    xhr.open('GET', icsUrl);
    xhr.send();
}

Pebble.addEventListener('ready', function () {
    fetchWeather();
    fetchCalendar();
});

Pebble.addEventListener('appmessage', function (e) {
    if (e.payload['REQUEST_DATA']) {
        fetchWeather();
        fetchCalendar();
    }
});

Pebble.addEventListener('showConfiguration', function () {
    Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function (e) {
    if (!e || !e.response) return;
    try {
        var decoded = decodeURIComponent(e.response);
        var settings = JSON.parse(decoded);
        var flat = {};
        var keys = Object.keys(settings);
        for (var i = 0; i < keys.length; i++) {
            var v = settings[keys[i]];
            flat[keys[i]] = (v && typeof v === 'object') ? v.value : v;
        }
        localStorage.setItem('clay-settings', JSON.stringify(flat));
        localStorage.setItem('temp_unit', flat.TEMP_UNIT ? '1' : '0');
        localStorage.setItem('ics_url', flat.ICS_URL || '');
    } catch (ex) {
        console.log('Settings error: ' + ex);
    }
    fetchWeather();
    fetchCalendar();
});
