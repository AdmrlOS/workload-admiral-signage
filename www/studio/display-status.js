// Device navigation comes from the native browser, never the studio preview.
(function(root) {
  function displayStatus(status, config) {
    if (status.error) return {tone:'neutral',title:'Device status unavailable',detail:'The device could not report its display status. The browser preview is separate from the physical display.'};
    const uri = status.browser?.uri;
    if (!uri) return {tone:'neutral',title:'Browser preview only',detail:'No device display is reporting here. This preview follows the published loop.'};
    let follows = false;
    try {
      const actual = new URL(uri), expected = new URL(config.player_url || 'http://127.0.0.1:8080/player/');
      const loopback = (host) => ['127.0.0.1','localhost','[::1]'].includes(host);
      follows = actual.protocol === expected.protocol && actual.port === expected.port &&
        (actual.hostname === expected.hostname || (loopback(actual.hostname) && loopback(expected.hostname))) &&
        actual.pathname.replace(/\/$/,'') === '/player';
    } catch (_) { /* An unknown address cannot confirm the scheduled player. */ }
    if (follows) return {tone:'',title:'Device uses the scheduled player',detail:'Published changes are checked every two seconds. The preview and device play independently.'};
    return {tone:'warning',title:'Device display is not following this loop',detail:'The physical display is running a standalone page. Publishing updates the loop, but that page will keep showing its own content.',help:true};
  }
  if (typeof module !== 'undefined' && module.exports) module.exports = displayStatus;
  else root.signageDisplayStatus = displayStatus;
})(typeof window === 'undefined' ? globalThis : window);
