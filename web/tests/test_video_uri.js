const fs = require('fs');
// The page's video URI rule must agree with apcam_video_uri_valid() (include/apcam/network.h).
const src = fs.readFileSync(process.argv[2], 'utf8');
const start = src.indexOf('const videoUri = text => {');
const end = src.indexOf('const number = text =>');
const videoUri = eval('(' + src.slice(start + 'const videoUri = '.length, end).trim().replace(/;$/, '') + ')');
const valid = ["rtsp://10.0.0.5:8554/video1", "rtsps://pi.local/stream", "rtsp://pi-1.example.com",
  "rtsp://192.168.1.2:1/a/b.264", "rtsp://host:65535/", "rtsp://192.168.144.20:8554"];
const invalid = ["", "10.0.0.5", "http://10.0.0.5/video1", "RTSP://10.0.0.5/video1", "rtsp://",
  "rtsp:///video1", "rtsp://:8554/video1", "rtsp://host:/video1", "rtsp://host:0/video1",
  "rtsp://host:65536/video1", "rtsp://host:123456/video1", "rtsp://host:80x/video1",
  "rtsp://user:pass@host/video1", "rtsp://host?x=1", "rtsp://host/has space",
  'rtsp://host/"quoted"', "rtsp://host/café", "rtsp://[::1]:8554/video1", "rtsp://192.168.144.20:blah"];
const longest = 'rtsp://h/' + 'a'.repeat(159 - 9), tooLong = longest + 'a';
let bad = 0;
for (const u of valid.concat([longest])) if (!videoUri(u)) { console.log('SHOULD BE VALID:', u); bad++; }
for (const u of invalid.concat([tooLong])) if (videoUri(u)) { console.log('SHOULD BE INVALID:', JSON.stringify(u)); bad++; }
console.log(bad ? 'FAIL video URI page rule: ' + bad + ' mismatches' : 'PASS video URI page rule matches the C validator (' + (valid.length + invalid.length + 2) + ' cases)');
process.exit(bad ? 1 : 0);
