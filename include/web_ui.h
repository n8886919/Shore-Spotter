#pragma once
// Embedded single-page web UI served over WiFi AP.
// Leaflet.js loads from CDN (phone keeps cellular data while on ESP32 AP).
static const char WEB_UI_HTML[] = R"rawlit(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>Shore Spotter</title>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css"/>
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<style>
html,body{margin:0;height:100%;font-family:Arial,sans-serif}
#map{height:calc(100% - 52px)}
#bar{
  height:52px;background:#0d47a1;color:#fff;
  display:flex;align-items:center;padding:0 12px;
  gap:16px;font-size:13px;white-space:nowrap;overflow-x:auto
}
b{font-weight:700}
.on{color:#69f0ae}
.off{color:#ff5252}
</style>
</head>
<body>
<div id="map"></div>
<div id="bar">
  <span>Link <b id="ls" class="off">--</b></span>
  <span>Brg <b id="brg">--</b>&deg;</span>
  <span>Batt <b id="bat">--</b>&nbsp;mV</span>
  <span>RX <b id="age">--</b>s</span>
  <span>Track <b id="pts">0</b>pts</span>
</div>
<script>
var map=L.map('map').setView([25.04,121.5],15);
L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',{
  attribution:'&copy; <a href="https://openstreetmap.org">OpenStreetMap</a>',
  maxZoom:19
}).addTo(map);

var cm=null,sm=null,bl=null,moved=false;
var trk=L.polyline([],{color:'#f44336',weight:3,opacity:0.8}).addTo(map);

function mk(c){
  return L.circleMarker([0,0],{radius:9,color:c,fillColor:c,fillOpacity:0.9,weight:2});
}

setInterval(function(){
  fetch('/api/track').then(function(r){return r.json();}).then(function(d){
    var ok=d.linked;
    var ls=document.getElementById('ls');
    ls.textContent=ok?'ONLINE':'NO SIGNAL';
    ls.className=ok?'on':'off';
    document.getElementById('brg').textContent=
      d.bearing>=0?d.bearing.toFixed(0)+'deg':'--';
    document.getElementById('bat').textContent=
      d.client.batt_mv>0?d.client.batt_mv:'--';
    document.getElementById('age').textContent=
      d.client.last_rx_sec>=0?d.client.last_rx_sec:'--';
    document.getElementById('pts').textContent=(d.history||[]).length;

    if(d.client.fix){
      var ll=[d.client.lat,d.client.lon];
      if(!cm){cm=mk('#f44336').addTo(map).bindPopup('Client (water)');}
      cm.setLatLng(ll);
      if(!moved){map.setView(ll,16);moved=true;}
    }

    if(d.server.fix){
      var sl=[d.server.lat,d.server.lon];
      if(!sm){sm=mk('#1565c0').addTo(map).bindPopup('Shore station');}
      sm.setLatLng(sl);
      if(d.client.fix&&d.bearing>=0){
        if(bl)map.removeLayer(bl);
        bl=L.polyline([sl,[d.client.lat,d.client.lon]],
          {color:'#ff8f00',weight:2,dashArray:'6 4'}).addTo(map);
      }
    }

    var h=d.history||[];
    if(h.length>0){
      trk.setLatLngs(h.map(function(p){return[p.lat,p.lon];}));
    }
  }).catch(function(){});
},2000);
</script>
</body>
</html>
)rawlit";
