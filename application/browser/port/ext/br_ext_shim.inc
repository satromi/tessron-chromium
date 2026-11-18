/*
 * The chrome object of an extension, in JavaScript: 'call(op, json)' answers
 * a promise of the answer's JSON. A content script's and an extension page's
 * have the native function of their frame behind them, the worker's a fetch
 * of its own origin. The native side checks what the extension declared; here
 * a name it did not declare is simply not there, as in Chrome.
 * What happens elsewhere (a message, an alarm, a request seen) waits in a log
 * of the browser's until this context's pump asks for what came since.
 */
#define BR_EXT_MAKE R"JS(
function brextMake(call, id, manifest, info, kind) {
  var base = 'chrome-extension://' + id + '/', isContent = kind === 'content';
  var sid = kind + Math.random(), has = function (p) { return info.perms.indexOf(p) >= 0; };
  var ch = {};
  var j = function (op, arg) {
    return call(op, arg === undefined ? '' : JSON.stringify(arg)).then(function (s) {
      var o = s === '' ? undefined : JSON.parse(s);
      if (o && o.__err) throw new Error(o.__err);
      return o;
    });
  };
  var cb = function (p, f) {
    if (typeof f !== 'function') return p;
    p.then(function (v) { f(v); }, function (e) {
      ch.runtime.lastError = { message: e.message }; try { f(); } finally { ch.runtime.lastError = undefined; }
    });
    return undefined;
  };
  var wait = function (ms) { return new Promise(function (ok) { setTimeout(ok, ms); }); };
  var pumping = false, since = kind === 'worker' ? 0 : -1;
  function pumpOn() { if (!pumping) { pumping = true; pump(); } }
  var event = function () {
    var l = [];
    return { addListener: function (f) { if (l.indexOf(f) < 0) l.push(f); pumpOn(); },
             removeListener: function (f) { var i = l.indexOf(f); if (i >= 0) l.splice(i, 1); },
             hasListener: function (f) { return l.indexOf(f) >= 0; }, hasListeners: function () { return l.length > 0; }, _l: l };
  };
  var fire = function (ev) {
    var a = Array.prototype.slice.call(arguments, 1);
    ev._l.slice().forEach(function (f) { try { f.apply(null, a); } catch (e) { console.error(e); } });
  };
  function answered(mid) {
    return (async function () {
      for (var t = 0; t < 600; t++) {
        var a = await j('msg.answer', { id: mid });
        if (a && a.has) return a.response === null ? undefined : a.response;
        await wait(50);
      }
      throw new Error('Could not establish connection. Receiving end does not exist.');
    })();
  }
  var onMessage = event(), onInstalled = event(), onStartup = event(), onChanged = event();
  function deliver(env) {
    var done = false, keep = false, l = onMessage._l;
    var respond = function (r) { if (!done) { done = true; j('msg.reply', { id: env.id, response: r === undefined ? null : r }); } };
    var sender = { id: id, url: env.url, origin: env.origin };
    if (env.tab) { sender.tab = env.tab; sender.frameId = env.frameId; }
    l.slice().forEach(function (f) { try { if (f(env.msg, sender, respond) === true) keep = true; } catch (e) { console.error(e); } });
    /* a page with no one listening leaves the answer to the worker */
    if (!keep && (kind !== 'page' || l.length > 0)) respond(null);
  }
  var areas = {}, webs = {}, events = {};
  function dispatch(env) {
    if (env.sid === sid) return;
    if (env.ev === 'storage') {
      fire(onChanged, env.changes, env.area);
      if (areas[env.area]) fire(areas[env.area].onChanged, env.changes);
    } else if (env.ev === 'web') {
      var w = webs[env.name];
      if (w) w.each(env.details);
    } else if (env.ev) {
      var e = events[env.ev];
      if (e) fire.apply(null, [e].concat(env.args || []));
    } else {
      deliver(env);
    }
  }
  function pump() {
    /*
     * the question (the worker's fetch, a content script's or a page's native call, whose promise is settled later)
     * waits on the browser's side until something comes: asked again at once; a refusal after a second
     */
    j('msg.poll', { since: since }).then(function (o) { since = o.seq; o.items.forEach(dispatch); return 0; },
                                         function () { return 1000; }).then(function (ms) {
      setTimeout(pump, ms);
    });
  }
  function send(to, msg) {
    return j('msg.send', { to: to, msg: msg === undefined ? null : msg, sid: sid, url: self.location.href,
                           origin: self.location.origin, frameId: info.frameId || 0 }).then(answered);
  }
  var area = function (name) {
    var a = {
      get: function (k, f) { if (typeof k === 'function') { f = k; k = undefined; } return cb(j('storage.get', { area: name, keys: k === null ? undefined : k }), f); },
      getKeys: function (f) { return cb(j('storage.keys', { area: name }), f); },
      getBytesInUse: function (k, f) { if (typeof k === 'function') f = k; return cb(Promise.resolve(0), f); },
      set: function (o, f) { return cb(j('storage.set', { area: name, items: o, sid: sid }), f); },
      remove: function (k, f) { return cb(j('storage.remove', { area: name, keys: k, sid: sid }), f); },
      clear: function (f) { return cb(j('storage.clear', { area: name, sid: sid }), f); },
      setAccessLevel: function (o, f) { return cb(Promise.resolve(), f); },
      onChanged: event()
    };
    areas[name] = a;
    return a;
  };
  var msgOf = function (name, subs) {
    var m = info.messages[String(name).toLowerCase()], lang = info.lang;
    var special = { '@@extension_id': id, '@@ui_locale': lang, '@@bidi_dir': 'ltr', '@@bidi_reversed_dir': 'rtl',
                    '@@bidi_start_edge': 'left', '@@bidi_end_edge': 'right' };
    if (special[String(name).toLowerCase()]) return special[String(name).toLowerCase()];
    if (!m) return '';
    var list = subs === undefined || subs === null ? [] : Array.isArray(subs) ? subs : [subs];
    var num = function (s) { return s.replace(/\$(\d)/g, function (a, n) { return list[n - 1] === undefined ? '' : String(list[n - 1]); }); };
    var ph = m.placeholders || {};
    return num(String(m.message).replace(/\$([A-Za-z0-9_@]+)\$/g, function (a, n) {
      var p = ph[n.toLowerCase()];
      return p ? String(p.content) : a;
    })).replace(/\$\$/g, '$');
  };
  ch.runtime = {
    id: id, lastError: undefined,
    getURL: function (p) { return base + String(p).replace(/^\//, ''); },
    getManifest: function () { return JSON.parse(JSON.stringify(manifest)); },
    sendMessage: function (a, b, c, d) {
      var f = [a, b, c, d].filter(function (x) { return typeof x === 'function'; })[0];
      var m = typeof a === 'string' && b !== undefined && typeof b !== 'function' && a === id ? b : a;
      return cb(send('ext', m), f);
    },
    onMessage: onMessage, onInstalled: onInstalled, onStartup: onStartup,
    onSuspend: event(), onMessageExternal: event(), onConnect: event(), onUpdateAvailable: event()
  };
  ch.i18n = {
    getMessage: msgOf,
    getUILanguage: function () { return info.lang; },
    getAcceptLanguages: function (f) { return cb(Promise.resolve([info.lang, 'en-US', 'en']), f); },
    detectLanguage: function (t, f) { return cb(Promise.resolve({ isReliable: false, languages: [] }), f); }
  };
  if (has('storage')) {
    ch.storage = { local: area('local'), session: area('session'), sync: area('sync'), onChanged: onChanged,
                   managed: { get: function (k, f) { if (typeof k === 'function') f = k; return cb(Promise.resolve({}), f); }, onChanged: event() } };
  }
  if (!isContent) {
    ch.runtime.getPlatformInfo = function (f) { return cb(Promise.resolve({ os: 'tessronos', arch: info.arch, nacl_arch: info.arch }), f); };
    ch.runtime.openOptionsPage = function (f) { return cb(j('runtime.options'), f); };
    ch.runtime.setUninstallURL = function (u, f) { return cb(Promise.resolve(), f); };
    ch.runtime.reload = function () { j('runtime.reload'); };
    events.tabUpdated = event(); events.tabRemoved = event(); events.tabActivated = event();
    ch.tabs = {
      TAB_ID_NONE: -1,
      query: function (q, f) { return cb(j('tabs.query', q || {}), f); },
      get: function (t, f) { return cb(j('tabs.query').then(function (l) { if (t !== l[0].id) throw new Error('No tab with id: ' + t); return l[0]; }), f); },
      getCurrent: function (f) { return cb(Promise.resolve(undefined), f); },
      create: function (o, f) { return cb(j('tabs.create', o), f); },
      update: function (a, b, c) { var o = typeof a === 'number' ? b : a; var r = typeof a === 'number' ? c : b; return cb(j('tabs.update', o), r); },
      reload: function (a, b, c) { var f = [a, b, c].filter(function (x) { return typeof x === 'function'; })[0]; return cb(j('tabs.reload'), f); },
      remove: function (t, f) { return cb(Promise.reject(new Error('The tab is the window: it is closed by its user')), f); },
      sendMessage: function (t, m, o, f) { if (typeof o === 'function') f = o; return cb(send('tab', m), f); },
      onUpdated: events.tabUpdated, onRemoved: events.tabRemoved, onActivated: events.tabActivated, onCreated: event(), onReplaced: event()
    };
    ch.windows = {
      WINDOW_ID_NONE: -1, WINDOW_ID_CURRENT: -2,
      get: function (w, a, f) { if (typeof a === 'function') f = a; return cb(Promise.resolve({ id: 1, focused: true, type: 'normal' }), f); },
      getCurrent: function (a, f) { if (typeof a === 'function') f = a; return cb(Promise.resolve({ id: 1, focused: true, type: 'normal' }), f); },
      getLastFocused: function (a, f) { if (typeof a === 'function') f = a; return cb(Promise.resolve({ id: 1, focused: true, type: 'normal' }), f); },
      getAll: function (a, f) { if (typeof a === 'function') f = a; return cb(Promise.resolve([{ id: 1, focused: true, type: 'normal' }]), f); },
      create: function (o, f) { return cb(j('tabs.create', { url: o && (Array.isArray(o.url) ? o.url[0] : o.url) }).then(function () { return { id: 2, type: o && o.type || 'normal' }; }), f); },
      update: function (w, o, f) { return cb(Promise.resolve({ id: w }), f); },
      onFocusChanged: event(), onCreated: event(), onRemoved: event()
    };
    ch.permissions = {
      getAll: function (f) { return cb(j('permissions.getAll'), f); },
      contains: function (o, f) { return cb(j('permissions.contains', o), f); },
      request: function (o, f) { return cb(j('permissions.request', o).then(function (r) { return typeof r === 'boolean' ? r : answered(r.wait); }), f); },
      remove: function (o, f) { return cb(j('permissions.remove', o), f); },
      onAdded: events.permAdded = event(), onRemoved: events.permRemoved = event()
    };
    /* the manifest's commands are there to be read; no key sets them off in this browser */
    ch.commands = { onCommand: event(), getAll: function (f) { return cb(Promise.resolve([]), f); } };
    if (manifest.action) {
      events.action = event();
      var setter = function (op) { return function (o, f) { return cb(j('action.set', { what: op, details: o }), f); }; };
      var getter = function (op) { return function (o, f) { if (typeof o === 'function') { f = o; o = {}; } return cb(j('action.get', { what: op }), f); }; };
      ch.action = {
        setTitle: setter('title'), getTitle: getter('title'), setBadgeText: setter('text'), getBadgeText: getter('text'),
        setBadgeBackgroundColor: setter('color'), getBadgeBackgroundColor: getter('color'), setBadgeTextColor: setter('textColor'), getBadgeTextColor: getter('textColor'),
        setPopup: setter('popup'), getPopup: getter('popup'), setIcon: setter('icon'),
        enable: function (t, f) { if (typeof t === 'function') f = t; return cb(j('action.set', { what: 'enabled', details: { enabled: true } }), f); },
        disable: function (t, f) { if (typeof t === 'function') f = t; return cb(j('action.set', { what: 'enabled', details: { enabled: false } }), f); },
        isEnabled: function (t, f) { if (typeof t === 'function') f = t; return cb(j('action.get', { what: 'enabled' }), f); },
        openPopup: function (o, f) { if (typeof o === 'function') f = o; return cb(j('action.open'), f); },
        getUserSettings: function (f) { return cb(Promise.resolve({ isOnToolbar: true }), f); },
        onClicked: events.action
      };
    }
    if (has('alarms')) {
      events.alarm = event();
      ch.alarms = {
        create: function (a, b, c) { var n = typeof a === 'string' ? a : ''; var o = typeof a === 'string' ? b : a; var f = [a, b, c].filter(function (x) { return typeof x === 'function'; })[0]; return cb(j('alarms.create', { name: n, info: o }), f); },
        get: function (a, b) { var n = typeof a === 'string' ? a : ''; return cb(j('alarms.get', { name: n }), typeof a === 'function' ? a : b); },
        getAll: function (f) { return cb(j('alarms.getAll'), f); },
        clear: function (a, b) { var n = typeof a === 'string' ? a : ''; return cb(j('alarms.clear', { name: n }), typeof a === 'function' ? a : b); },
        clearAll: function (f) { return cb(j('alarms.clearAll'), f); },
        onAlarm: events.alarm
      };
    }
    if (has('contextMenus')) {
      events.menu = event();
      ch.contextMenus = {
        ACTION_MENU_TOP_LEVEL_LIMIT: 6,
        create: function (o, f) {
          var i = o.id === undefined ? 'auto' + Math.random() : o.id;
          var c = Object.assign({}, o, { id: i });
          j('contextMenus.create', c).then(function () { if (f) f(); }, function (e) { ch.runtime.lastError = { message: e.message }; if (f) f(); ch.runtime.lastError = undefined; });
          return i;
        },
        update: function (i, o, f) { return cb(j('contextMenus.update', { id: i, props: o }), f); },
        remove: function (i, f) { return cb(j('contextMenus.remove', { id: i }), f); },
        removeAll: function (f) { return cb(j('contextMenus.removeAll'), f); },
        onClicked: events.menu
      };
    }
    if (has('scripting')) {
      ch.scripting = {
        executeScript: function (o, f) {
          var a = Object.assign({}, o); if (typeof a.func === 'function') a.func = a.func.toString();
          return cb(j('scripting.exec', a).then(answered), f);
        },
        insertCSS: function (o, f) { return cb(j('scripting.css', Object.assign({ remove: false }, o)).then(answered), f); },
        removeCSS: function (o, f) { return cb(j('scripting.css', Object.assign({ remove: true }, o)).then(answered), f); },
        registerContentScripts: function (l, f) { return cb(j('scripting.register', { scripts: l }), f); },
        updateContentScripts: function (l, f) { return cb(j('scripting.update', { scripts: l }), f); },
        unregisterContentScripts: function (o, f) { if (typeof o === 'function') { f = o; o = {}; } return cb(j('scripting.unregister', o || {}), f); },
        getRegisteredContentScripts: function (o, f) { if (typeof o === 'function') { f = o; o = {}; } return cb(j('scripting.registered', o || {}), f); },
        ExecutionWorld: { ISOLATED: 'ISOLATED', MAIN: 'MAIN' }, StyleOrigin: { AUTHOR: 'AUTHOR', USER: 'USER' }
      };
    }
    if (has('webRequest')) {
      var pat = function (p) {
        if (p === '<all_urls>') return /^(https?|wss?|ftp|file):/;
        var re = p.replace(/[.+?^${}()|[\]\\]/g, '\\$&').replace(/\*/g, '.*').replace(/^\.\*:/, '(https?|wss?):').replace(/:\/\/\.\*\\\./, '://([^/]*\\.)?');
        return new RegExp('^' + re + '$');
      };
      var web = function (name) {
        var l = [];
        var e = {
          addListener: function (f, filter, extra) {
            /* a manifest V3 extension only observes: nothing it answers can hold or change a request */
            if (extra && extra.indexOf('blocking') >= 0) throw new Error("'webRequestBlocking' permission is required.");
            if (!filter || !Array.isArray(filter.urls)) throw new Error('webRequest: a filter with urls is required');
            l.push({ f: f, urls: filter.urls.map(pat), types: filter.types, extra: extra || [] });
            j('web.listen'); pumpOn();
          },
          removeListener: function (f) { for (var i = l.length; i-- > 0;) if (l[i].f === f) l.splice(i, 1); },
          hasListener: function (f) { return l.some(function (x) { return x.f === f; }); },
          each: function (d) {
            l.slice().forEach(function (x) {
              if (x.types && x.types.indexOf(d.type) < 0) return;
              if (!x.urls.some(function (r) { return r.test(d.url); })) return;
              var o = Object.assign({}, d);
              if (x.extra.indexOf('responseHeaders') < 0) delete o.responseHeaders;
              if (x.extra.indexOf('requestHeaders') < 0) delete o.requestHeaders;
              try { x.f(o); } catch (er) { console.error(er); }
            });
          }
        };
        webs[name] = e;
        return e;
      };
      ch.webRequest = { MAX_HANDLER_BEHAVIOR_CHANGED_CALLS_PER_10_MINUTES: 20,
                        handlerBehaviorChanged: function (f) { return cb(Promise.resolve(), f); } };
      ['onBeforeRequest', 'onBeforeSendHeaders', 'onSendHeaders', 'onHeadersReceived', 'onAuthRequired', 'onResponseStarted',
       'onBeforeRedirect', 'onCompleted', 'onErrorOccurred'].forEach(function (n) { ch.webRequest[n] = web(n); });
    }
    if (has('declarativeNetRequest') || has('declarativeNetRequestWithHostAccess')) {
      var failed = function (op) { return function (o, f) { return cb(j(op, o), f); }; };
      var listed = function (op) { return function (a, b) { var f = typeof a === 'function' ? a : b; return cb(j(op, typeof a === 'function' ? undefined : a), f); }; };
      ch.declarativeNetRequest = {
        GUARANTEED_MINIMUM_STATIC_RULES: 30000, MAX_NUMBER_OF_STATIC_RULESETS: 100, MAX_NUMBER_OF_ENABLED_STATIC_RULESETS: 50,
        MAX_NUMBER_OF_DYNAMIC_RULES: 30000, MAX_NUMBER_OF_SESSION_RULES: 5000, MAX_NUMBER_OF_REGEX_RULES: 1000,
        MAX_NUMBER_OF_UNSAFE_DYNAMIC_RULES: 5000, MAX_NUMBER_OF_UNSAFE_SESSION_RULES: 5000,
        DYNAMIC_RULESET_ID: '_dynamic', SESSION_RULESET_ID: '_session',
        updateDynamicRules: failed('dnr.updateDynamic'), getDynamicRules: listed('dnr.getDynamic'),
        updateSessionRules: failed('dnr.updateSession'), getSessionRules: listed('dnr.getSession'),
        updateEnabledRulesets: failed('dnr.updateEnabled'), getEnabledRulesets: listed('dnr.getEnabled'),
        updateStaticRules: failed('dnr.updateStatic'), getDisabledRuleIds: failed('dnr.getDisabled'),
        isRegexSupported: failed('dnr.regexOk'),
        getAvailableStaticRuleCount: function (f) { return cb(Promise.resolve(300000), f); },
        getMatchedRules: function (a, b) { var f = typeof a === 'function' ? a : b; return cb(Promise.resolve({ rulesMatchedInfo: [] }), f); },
        setExtensionActionOptions: function (o, f) { return cb(Promise.resolve(), f); }
      };
    }
  }
  if (kind === 'worker') {
    pumpOn();
    j('runtime.first').then(function (first) {
      /* after the script's first turn, when its listeners are there */
      setTimeout(function () {
        if (first) fire(onInstalled, { reason: 'install' });
        else fire(onStartup);
      }, 0);
    });
  }
  return ch;
}
)JS"

/* In a content script's world or an extension page: (native, id, manifest, info, kind) */
static const char kExtContentShim[] = "(function () {" BR_EXT_MAKE R"JS(
  return function (native, id, manifest, info, kind) {
    globalThis.chrome = brextMake(function (op, arg) {
      try { return Promise.resolve(native(op, arg)); } catch (e) { return Promise.reject(e); }
    }, id, JSON.parse(manifest), JSON.parse(info), kind);
    globalThis.browser = globalThis.chrome;
  };
})())JS";

/* In front of the worker's script; __ID__, __MANIFEST__ and __INFO__ are its own */
static const char kExtWorkerShim[] = "(function () {" BR_EXT_MAKE R"JS(
  self.chrome = brextMake(function (op, arg) {
    return fetch('/__brext/' + op, { method: 'POST', body: arg }).then(function (r) { return r.text(); });
  }, '__ID__', __MANIFEST__, __INFO__, 'worker');
  self.browser = self.chrome;
})();
)JS";
