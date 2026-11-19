/*

 server MIDI viewer web server (express)

 ©2026 by D.F.Mac.@TripArts Music
 Programmed by Claude Code

 ver. history

 - 2026/11/18 : Start Writing

*/
// Web MIDI requires a secure context. localhost counts as secure, so it works over plain http (Chrome / Edge).
const express = require('express');
const path = require('path');
const PORT = process.env.PORT || 3000;
const app = express();
app.use(express.static(path.join(__dirname, 'public')));
app.listen(PORT, '127.0.0.1', () => console.log(`MIDI trigger viewer: http://localhost:${PORT}  (Chrome で開く)`));
