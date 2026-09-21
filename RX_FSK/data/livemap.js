try {
  var check = $(document);
} catch (e) {
  document.addEventListener("DOMContentLoaded", function(event) {
    document.getElementById('map').innerHTML = '<br /><br />In order to use this functionality, there must be an internet connection.<br /><br/><a href="livemap.html">retry</a><br /><br/><a href="index.html">go back</a>';
  });
}

$(document).ready(function(){

  var map = L.map('map', { attributionControl: false, zoomControl: false });
  map.on('mousedown touchstart',function () { follow=false; });

  L.control.scale().addTo(map);
  L.control.attribution({prefix:false}).addTo(map);
  
  var osmlight = L.tileLayer('https://{s}.tile.openstreetmap.de/{z}/{x}/{y}.png', {
  	maxZoom: 19,
  	attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors'
  });

  var osmdark = L.tileLayer('https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}.png', {
    attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors &copy; <a href="https://carto.com/attributions">CARTO</a>',
    maxZoom: 19
  });
  
  var opentopo = L.tileLayer('https://{s}.tile.opentopomap.org/{z}/{x}/{y}.png', {
  	maxZoom: 17,
  	attribution: 'Map data: &copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors, <a href="http://viewfinderpanoramas.org">SRTM</a><br />Map style: &copy; <a href="https://opentopomap.org">OpenTopoMap</a> (<a href="https://creativecommons.org/licenses/by-sa/3.0/">CC-BY-SA</a>)'
  });

  var esri = L.tileLayer('https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}', {
  	attribution: 'Tiles &copy; Esri &mdash; Source: Esri, i-cubed, USDA, USGS, AEX, GeoEye,<br />Getmapping, Aerogrid, IGN, IGP, UPR-EGP, and the GIS User Community',
    maxZoom: 21
  });
  
  var basemap;
  var themeDark = (window.rdzTheme && window.rdzTheme.effective() === 'dark')
               || (!window.rdzTheme && window.matchMedia
                   && window.matchMedia('(prefers-color-scheme: dark)').matches);
  if (themeDark) {
    map.addLayer(osmdark);
    basemap='osmdark';
  } else {
    map.addLayer(osmlight);
    basemap='osmlight';
  }

  basemap_change = function () {
    if (basemap == 'osmlight') {
      map.removeLayer(osmlight);
      map.addLayer(opentopo);
      basemap = 'opentopo';
    } else if (basemap == 'opentopo') {
      map.removeLayer(opentopo);
      map.addLayer(esri);
      basemap = 'esri';
    } else if (basemap == 'esri') {
      map.removeLayer(esri);
      map.addLayer(osmdark);
      basemap = 'osmdark';
    } else {
      map.removeLayer(osmdark);
      map.addLayer(osmlight);
      basemap = 'osmlight';
    }
  };

  /* Follow the theme switch, but only while the user is on a plain OSM layer --
     if they deliberately picked opentopo or esri, leave their choice alone. */
  if (window.rdzTheme) {
    window.rdzTheme.onChange(function (theme) {
      var want = (theme === 'dark') ? 'osmdark' : 'osmlight';
      if (basemap === want) return;
      if (basemap !== 'osmlight' && basemap !== 'osmdark') return;
      map.removeLayer(basemap === 'osmlight' ? osmlight : osmdark);
      map.addLayer(want === 'osmdark' ? osmdark : osmlight);
      basemap = want;
    });
  }

  if(mapcenter) map.setView(mapcenter, 5);
  else map.setView([51.163361,10.447683], 5); // Mitte DE

var reddot = '<span class="ldot rbg"></span>';
var yellowdot = '<span class="ldot ybg"></span>';
var greendot = '<span class="ldot gbg"></span>';
var lastframe = 0;

$('#map .leaflet-control-container').append(L.DomUtil.create('div', 'leaflet-top leaflet-center leaflet-header'));
var header = '';
header += '<div id="sonde_main"><b>rdzTTGOSonde LiveMap</b><br />🎈 <b><span id="sonde_id"></span> - <span id="sonde_freq"></span> MHz - <span id="sonde_type"></span></b></div>';
header += '<div id="sonde_detail"><span id="sonde_alt"></span>m | <span id="sonde_climb"></span>m/s | <span id="sonde_speed"></span>km/h | <span id="sonde_dir"></span>°<br /><span id="sonde_time"></span> | -<span id="sonde_rssi"></span>dBm</div>';
header += '<div id="sonde_status"><span id="sonde_statbar"></span></div>';
header += '<div id="settings"><br /><b>Prediction-Settings</b><br />';

header += '<label for="burst">Burst at:</label><input type="text" id="burst" maxlength="5" value="..."  /> m<br />';
header += '<label for="overwrite_descend">Descending:</label><input type="text" id="overwrite_descend" maxlength="2" value="..."  /> m/s<br />';
header += '<label for="overwrite_descend_till">Use this descending until:</label><input type="text" id="overwrite_descend_till" maxlength="5" value="..."  /> m<br />';
header += '<small>after the transmitted descend will be used</small>';
header += '<div id="submit"><input type="button" value="save" onclick="settings_save();"/>&nbsp;&nbsp;&nbsp;<input type="button" id="submit" value="reset" onclick="settings_reset();"/></div>';
header += '</div>';
$('.leaflet-header').append(header);


$('#map .leaflet-control-container').append(L.DomUtil.create('div', 'leaflet-bottom leaflet-center leaflet-footer'));
var footer = '';
footer += '<div id="gps_main"><b>Direction: </b><span class="gps_dir">...</span>°<br /><b>Distance: </b><span class="gps_dist">...</span>m</div>';
$('.leaflet-footer').append(footer);

var statbar = '';
headtxt = function(data,stat) {
  var staticon = (stat == '1')?greendot:yellowdot; 
  statbar = staticon + statbar;
  if ((statbar.length) > 10*greendot.length) { statbar = statbar.substring(0,10*greendot.length); }
  if (data.id && data.vframe != lastframe ) {
    lastframe = data.vframe;
    $('#sonde_id').html(data.id);
    $('#sonde_alt').html(data.alt);
    $('#sonde_climb').html(data.climb);
    $('#sonde_speed').html( mr(data.speed * 3.6 * 10) / 10 );
    $('#sonde_dir').html(data.dir);
    $('#sonde_time').html(localtime(data.time));
    $('#sonde_rssi').html(data.rssi / 2 );
    $('#sonde_detail').show();
  } else {
    if (!data.id) {
    $('#sonde_id').html(data.launchsite.trim());
    // $('#sonde_detail').hide();
    }
  }
  $('#sonde_freq').html(data.freq);
  $('#sonde_type').html(data.type);
  $('#sonde_statbar').html('&nbsp;'+statbar);
};

map.addControl(new L.Control.Button([ { position: 'topleft', text: '🔙', href: 'index.html' } ]));
  
L.control.zoom({ position:'topleft' }).addTo(map);

map.addControl(new L.Control.Button([ { position: 'topleft', text: '🗺️', href: 'javascript:basemap_change();' } ]));

map.addControl(new L.Control.Button([ { position: 'topright', id: "status", text: '', href: 'javascript:get_data();' } ]));

map.addControl(new L.Control.Button([
  { position:'topright', text: '🎈', href: 'javascript:show(marker[last_id],\'marker\');' },
  { text: '〰️', href: 'javascript:show_line();' },
  { text: '💥', href: 'javascript:show(marker_burst[last_id],\'burst\');' },
  { text: '🎯', href: 'javascript:show(marker_landing[last_id],\'landing\');' }
]));

map.addControl(new L.Control.Button([ { position:'topright', text: '⚙️', href: 'javascript:show_settings();' } ]));

  /* The floating fallback would sit on top of Leaflet's own controls: every corner of this map
     is occupied (back/zoom/basemap topleft, status/balloon/settings topright, scale bottomleft,
     attribution bottomright). Hand the switch to Leaflet's control layout instead, so it is
     placed in flow below the top-right stack and cannot overlap anything. */
  var themeSw = document.getElementById('themeSwitch');
  var trCorner = document.querySelector('#map .leaflet-control-container .leaflet-top.leaflet-right');
  if (themeSw && trCorner) {
    themeSw.classList.remove('themetoggle-floating');
    var themeBox = L.DomUtil.create('div', 'leaflet-control');
    themeBox.appendChild(themeSw);
    trCorner.appendChild(themeBox);
    L.DomEvent.disableClickPropagation(themeBox);
    L.DomEvent.disableScrollPropagation(themeBox);
  }



  show = function(e,p) {
    if (p == 'landing') { get_predict(last_data); }
    if (e) {
      map.closePopup();
      map.setView(map._layers[e._leaflet_id].getLatLng());
      map._layers[e._leaflet_id].openPopup();
      follow = p;
    }
  };


  getTwoBounds = function (a,b) {
    var sW = new L.LatLng((a._southWest.lat > b._southWest.lat)?b._southWest.lat:a._southWest.lat, (a._southWest.lng > b._southWest.lng)?b._southWest.lng:a._southWest.lng);
    var nE = new L.LatLng((a._northEast.lat < b._northEast.lat)?b._northEast.lat:a._northEast.lat, (a._northEast.lng < b._northEast.lng)?b._northEast.lng:a._northEast.lng);

    return new L.LatLngBounds(sW, nE);
  };

  show_line = function() {
      $('.i_position, .i_landing').remove();
      map.closePopup();
      if (line[last_id]._latlngs.length != 0 && line_predict[last_id]._latlngs.length != 0) {
        map.fitBounds(getTwoBounds(line[last_id].getBounds(),line_predict[last_id].getBounds()));
      } else if (line[last_id]._latlngs.length != 0) {
        map.fitBounds(line[last_id].getBounds());
      } else if (line_predict[last_id]._latlngs.length != 0) {
        map.fitBounds(line_predict[last_id].getBounds());
      }
  };



  last_data = false;
  last_id = false;
  follow = 'marker';

  marker_landing = [];
  icon_landing = L.divIcon({className: 'leaflet-landing'});
  dots_predict = [];
  line_predict = [];
  marker_burst = [];
  icon_burst = L.divIcon({className: 'leaflet-burst'});
  poweroff = {};   // cached absolute power-off time (s epoch) per sonde id

  marker = [];
  dots = [];
  line = [];

  draw = function(data) {
    var stat;
    if (data.id) {
      last_id = data.id;
      // data.res: 0: ok  1: no rx (timeout), 2: crc err, >2 some other error
      // Skip plotting when the firmware flags the position as old (validPos & 0x80):
      // on weak signal a frame number can advance without a fresh fix, and the kept
      // (older) position must not be plotted as if it were the newest frame. Firmware
      // without validPos yields (undefined & 0x80)==0, so nothing is skipped there.
      if ((data.lat && data.lon && data.alt) && (lastframe != 0) && !(data.validPos & 0x80)) {
        var location = [data.lat,data.lon,data.alt];
        if (!marker[data.id]) {
          map.setView(location, 14);
          marker[data.id] = L.marker(location).addTo(map)
          .bindPopup(poptxt('position',data),{closeOnClick:false, autoPan:false}).openPopup();
          get_predict(data);
        } else {
          marker[data.id].slideTo(location, {
              duration: 500,
              keepAtCenter: (follow=='marker')?true:false
          })
          .setPopupContent(poptxt('position',data));
        }
        if (!dots[data.id]) { dots[data.id] = []; }
        dots[data.id].push(location);
        if (!line[data.id]) {
          line[data.id] = L.polyline(dots[data.id], {sondeid: data.id}).addTo(map).on('click', line_click);
        } else {
          line[data.id].setLatLngs(dots[data.id]);
        }
        
      }
      if (data.res == 0) {
        storage_write(data);
        $('#status').html(greendot);
        stat = 1;
      } else {
        $('#status').html(yellowdot);
        stat = 0;
      }
      headtxt(data,stat);
      last_data = data;
    } else {
      $('#status').html(yellowdot);
      headtxt(data,0);
    }
  };


  marker_gps = false;
  icon_gps = L.divIcon({className: 'leaflet-gps'});
  circ_gps = false;

  gps = function(e) {
    gps_location = [e.lat,e.lon];
    gps_accuracy = e.hdop*2;

    if (last_data && last_data.lat != '0.000000') {
      if ($('.leaflet-footer').css('display') == 'none') { $('.leaflet-footer').show(); }

      var distance = Math.round(map.distance(gps_location,[last_data.lat, last_data.lon]));
      distance = (distance > 1000)?(distance / 1000) + 'k':distance;
      $('.leaflet-footer .gps_dist').html(distance);

      $('.leaflet-footer .gps_dir').html( bearing(gps_location,[last_data.lat, last_data.lon]) );
    }

    if (!marker_gps) {
      map.addControl(new L.Control.Button([{ position: 'topleft', text: '🛰️', href: 'javascript:show(marker_gps,\'gps\');' }]));

      marker_gps = L.marker(gps_location,{icon:icon_gps}).addTo(map)
      .bindPopup(poptxt('gps',e),{closeOnClick:false, autoPan:false});
      circ_gps = L.circle(gps_location, gps_accuracy).addTo(map);
    } else {
      marker_gps.slideTo(gps_location, {
          duration: 500,
          keepAtCenter: (follow=='gps')?true:false
      })
      .setPopupContent(poptxt('gps',e));
      circ_gps.slideTo(gps_location, { duration: 500 });
      circ_gps.setRadius(gps_accuracy);
    }
  };

  get_data = function() {
      $('#status').html(reddot);
      $.ajax({url: 'live.json', success: (function( data ) {
        if (typeof data != "object") { data = $.parseJSON(data); }
        if (data.sonde) {
          draw(data.sonde);
        } else {
          setTimeout(function() {$('#status').html(yellowdot);},100);
        }
        if (data.gps) {
          gps(data.gps);
        }
        }),
        timeout: 1000}
     );
  };
  
  storage = (typeof(Storage) !== "undefined")?true:false;
  
  settings_std = {
    burst: 32500,
    overwrite_descend: 6,
    overwrite_descend_till: 12000
  };

  settings_read = function() {
    if (storage) {
      if (sessionStorage.settings) {
        return JSON.parse(sessionStorage.settings); 
      } else {
        settings_write(settings_std);
        return settings_std;
      }
    } else {
      return settings_std;
    }
    return false;
  };
  
  settings_write = function (data) {
    if (storage) { 
      sessionStorage.settings = JSON.stringify(data);
      settings = data;
    }
  };
  
  settings = settings_read();
  
  settings_save = function() {
    settings.burst = parseInt($('#settings #burst').val());
    settings.overwrite_descend = parseInt($('#settings #overwrite_descend').val());
    settings.overwrite_descend_till = parseInt($('#settings #overwrite_descend_till').val());
    if (Number.isInteger(settings.burst) && Number.isInteger(settings.overwrite_descend) && Number.isInteger(settings.overwrite_descend_till)) {
      settings_write(settings);
      $("#settings").slideUp();
      get_predict(last_data);
    } else {
      showAlert('Error: only numeric values allowed!');
    }
  };

  settings_reset = function() {
    showConfirm('Reset to default?').then(function(ok) {
      if (!ok) return;
      settings_write(settings_std);
      show_settings();
    });
  };

  show_settings = function() {
    $('#settings #burst').val(settings.burst);
    $('#settings #overwrite_descend').val(settings.overwrite_descend);
    $('#settings #overwrite_descend_till').val(settings.overwrite_descend_till);
    $("#settings").slideToggle();
  };

  predictor = false;
  get_predict = function(data) {
    if (!data) { return; }
    var ascent = (data.climb > 0)? data.climb : 15;
    var descent = (data.climb > 0)? settings.overwrite_descend : data.climb * -1;

    var burst;
    if (data.climb > 0) {
      burst = (data.alt > settings.burst )?data.alt + 100 : settings.burst;
    } else {
      burst = parseInt(data.alt) + 7;
      if (data.alt > settings.overwrite_descend_till ) { descent = settings.overwrite_descend; }
    }

    var m = new Date(data.time * 1000);
    var datetime = m.toISOString().slice(0,19) + 'Z';
    var url = 'https://api.v2.sondehub.org/tawhiri';
    url += '?launch_latitude='+data.lat + '&launch_longitude='+tawhiri_lon(data.lon);
    url += '&launch_altitude='+data.alt + '&launch_datetime='+datetime;
    url += '&ascent_rate='+ascent + '&burst_altitude=' + burst + '&descent_rate='+descent;

    $.getJSON(url, function( prediction ) {
      draw_predict(prediction,data);
    });
  };

  draw_predict = function(prediction,data) {  
    var ascending = prediction.prediction[0].trajectory;
    var highest = ascending[ascending.length-1];
    var highest_location = [highest.latitude,sanitize_lon(highest.longitude)];

    var descending = prediction.prediction[1].trajectory;
    var landing = descending[descending.length-1];
    var landing_location = [landing.latitude,sanitize_lon(landing.longitude)];

    if (!marker_landing[data.id]) {
      marker_landing[data.id] = L.marker(landing_location,{icon: icon_landing}).addTo(map)
      .bindPopup(poptxt('landing',landing,data.id),{closeOnClick:false, autoPan:false});
    } else {
      marker_landing[data.id].slideTo(landing_location, {
          duration: 500,
          keepAtCenter: (follow=='landing')?true:false
      })
      .setPopupContent(poptxt('landing',landing,data.id));
    }

    dots_predict[data.id]=[];

    if (data.climb > 0) {
      ascending.forEach(p => dots_predict[data.id].push([p.latitude,sanitize_lon(p.longitude)]));

      if (!marker_burst[data.id]) {
        marker_burst[data.id] = L.marker(highest_location,{icon:icon_burst}).addTo(map).bindPopup(poptxt('burst',highest),{closeOnClick:false, autoPan:false});
      } else {
        marker_burst[data.id].slideTo(highest_location, {
          duration: 500,
          keepAtCenter: (follow=='burst')?true:false
        }).setPopupContent(poptxt('burst',highest));
      }
    }

    descending.forEach(p => dots_predict[data.id].push([p.latitude,sanitize_lon(p.longitude)]));
    
    if (!line_predict[data.id]) { 
      line_predict[data.id] = L.polyline(dots_predict[data.id],{color: 'yellow'}).addTo(map);
    } else {
      line_predict[data.id].setLatLngs(dots_predict[data.id]);
    }

    if (data.climb > 0) {
      predictor_time =  5 * 60; // ascending, every 5 min
    } else if (data.climb < 0 && data.alt > 5000) {
      predictor_time =  2 * 60; // descending, above 5km, every 2 min
    } else {
      predictor_time =  30; // descending, below 5km, every 30 sec
    }
    clearTimeout(predictor);
    predictor = setTimeout(function() {get_predict(last_data);}, predictor_time*1000);
  };

  sanitize_lon = function(lon) {
    if (lon > 180) { return lon - 360; }
    return lon;
  }
  tawhiri_lon = function(lon) {
    if (lon < 0) { return lon + 360; }
    return lon;
  }  

  // Receiver position for the "Route" link: prefer the live GPS fix, else fall back to
  // the configured/served map center (posInfo). Returns [lat, lon] or null.
  receiverpos = function() {
    if (typeof gps_location !== 'undefined' && gps_location && gps_location.length == 2) { return gps_location; }
    if (typeof mapcenter !== 'undefined' && mapcenter && mapcenter.length == 2) { return mapcenter; }
    return null;
  };

  // "Open in <external map>" link row, shared by the sonde/predict popups and the
  // trail-point popup (line_click) so all links stay defined in one place.
  openlinks = function(lat, lon) {
    var links = '<b>Open:</b> <a href="https://www.google.de/maps/?q='+lat+', '+lon+'" target="_blank">GMaps</a> | <a href="https://www.openstreetmap.org/?mlat='+lat+'&mlon='+lon+'&zoom=15" target="_blank">OSM</a> | <a href="https://topographic-map.com/world/?popup='+lat+','+lon+'&center='+lat+','+lon+'&zoom=15&base=5" target="_blank">Topo</a> | <a href="geo://'+lat+','+lon+'">GeoApp</a>';
    // Driving route from the receiver to this point (needs a known receiver position).
    var rx = receiverpos();
    if (rx) {
      links += ' | <a href="https://www.google.de/maps/dir/'+rx[0]+','+rx[1]+'/'+lat+','+lon+'" target="_blank">Route</a>';
    }
    return links;
  };

  // Popup content for a clicked trail point, built from that point's stored frame
  // (see line_click). Each field is guarded so partial frames still render.
  trailpoptxt = function(f) {
    var lat = Math.round(f.lat * 1000000) / 1000000;
    var lon = Math.round(sanitize_lon(f.lon) * 1000000) / 1000000;
    var rows = '<b>Position:</b> '+lat+',  '+lon+'<br />';
    if (f.alt || f.alt === 0)     { rows += '<b>Altitude:</b> '+mr(f.alt)+' m<br />'; }
    if (f.speed || f.speed === 0) { rows += '<b>Speed:</b> '+(mr(f.speed * 3.6 * 10) / 10)+' km/h'+((f.dir || f.dir === 0) ? ' '+f.dir+'°' : '')+'<br />'; }
    if (f.climb || f.climb === 0) { rows += '<b>Climb:</b> '+f.climb+' m/s<br />'; }
    if (f.rssi || f.rssi === 0)   { rows += '<b>Signal:</b> -'+(f.rssi / 2)+' dBm<br />'; }
    // f.time = the frame's GPS timestamp (when the sonde was at this point).
    if (f.time) { rows += '<b>Time:</b> '+localtime(f.time)+'<br />'; }
    return '<div class="i_position"><b>〰️ Trail point</b><br />'+rows+openlinks(lat,lon)+'</div>';
  };

  // Trail (polyline) click handler. e.latlng is the raw clicked coordinate; find the
  // stored frame nearest to it -- for the sonde whose trail was clicked (options.sondeid)
  // -- and show its telemetry. Session storage is the per-frame source of truth, so this
  // works for both the live trail and a trail restored after reboot.
  line_click = function(e) {
    var id = e.target.options.sondeid;
    var frames = storage_read();
    if (!frames) { return; }
    var best = null, bestd = Infinity;
    frames.forEach(function(f) {
      if (f.id == id && f.lat && f.lon) {
        var d = e.latlng.distanceTo(L.latLng(f.lat, f.lon));
        if (d < bestd) { bestd = d; best = f; }
      }
    });
    if (!best) { return; }
    L.popup({closeOnClick:true, autoPan:false})
      .setLatLng(L.latLng(best.lat, best.lon))
      .setContent(trailpoptxt(best))
      .openOn(map);
  };

  poptxt = function(t,i,id) {
    var lat_input = (i.id)?i.lat:i.latitude;
    var lon_input = sanitize_lon((i.id)?i.lon:i.longitude);

    var lat = Math.round(lat_input * 1000000) / 1000000;
    var lon = Math.round(lon_input * 1000000) / 1000000;

    var add =
    '<br /><b>Position:</b> '+lat+',  '+lon+'<br />'+openlinks(lat,lon);

    if (t == 'position') {
      // RS41 shutdown ("kill") timer: countKT = seconds until power-off as of
      // frame crefKT. While a kill-timer frame is fresh (crefKT > 0 and within
      // ~51 frames, as in aprs.cpp / SondeHub; 0xffff = disabled) anchor the
      // fixed absolute power-off moment. Anchor on the frame's OWN time (i.time,
      // the GPS epoch of frame vframe) -- NOT the browser clock: after signal
      // loss live.json keeps returning the last frame, so both i.time and rem
      // are frozen and i.time+rem stays put. Anchoring on now()+rem instead
      // would treat that stale frame as received "now", so the absolute time
      // drifted forward on every poll / page reload (the in-memory cache is
      // wiped by F5). i.time+rem is reload- and poll-stable, so recomputing it
      // every time is fine. Other sonde types never set these, so nothing shows.
      var off = '';
      if (i.crefKT > 0 && i.countKT != 65535 && i.time > 0
          && ((i.vframe - i.crefKT) & 0xffff) < 51) {
        var rem = i.countKT - ((i.vframe - i.crefKT) & 0xffff);
        if (rem < 0) { rem = 0; }
        poweroff[i.id] = i.time + rem;
      }
      // Derive the live countdown from that fixed moment so "in" and "at" always
      // agree, and "in" keeps ticking down between frames / after signal loss.
      if (poweroff[i.id]) {
        var left = poweroff[i.id] - Math.floor(new Date().getTime() / 1000);
        if (left < 0) { left = 0; }
        off = '<br /><b>🔌 Power off in:</b> ' + hms(left) + '<br />at ' + localtime(poweroff[i.id]);
      }
      return '<div class="i_position"><b>🎈 '+i.id+'</b>'+off+add+'</div>';
    }
    if (t == 'burst') { return '<div class="i_burst"><b>💥 Predicted Burst:</b><br />'+fdl(i.datetime)+' at '+mr(i.altitude)+'m'+add+'</div>'; }
    if (t == 'highest') { return '<div class="i_burst"><b>💥 Burst:</b> '+mr(i.altitude)+'m'+add+'</div>';}
    if (t == 'landing') {
      // append the cached power-off time (see the 'position' branch) right after
      // the landing time, so it can be compared against when the sonde shuts down.
      var off = poweroff[id] ? '<br /><b>🔌 Power off at:</b> ' + localtime(poweroff[id]) : '';
      return '<div class="i_landing"><b>🎯 Predicted Landing:</b><br />'+fdl(i.datetime)+' at '+mr(i.altitude)+'m'+off+add+'</div>';
    }
    if (t == 'gps') { return '<div class="i_gps">Position: '+(i.lat)+','+(i.lon)+'<br />Altitude: '+i.alt+'m<br />Speed: '+mr(i.speed * 3.6 * 10)/10+'km/h '+i.dir+'°<br />Sat: '+i.sat+' Hdop:'+(i.hdop/10)+'</div>'; }
  };

  // Format a predictor datetime string in the browser's local timezone
  // (full date/time + UTC offset).
  fdl = function(date) { return localtime(Date.parse(date) / 1000); };
  az = function(n) { return (n<10)?'0'+n:n; };
  mr = function(n) { return Math.round(n); };
  // Format a number of seconds as h:mm:ss (no modulo: see the localtime note).
  hms = function(secs) {
    var h = Math.floor(secs / 3600);
    var rem = secs - h * 3600;
    var m = Math.floor(rem / 60);
    return h + ':' + az(m) + ':' + az(rem - m * 60);
  };
  // Format a unix epoch (seconds) as local date/time in the viewing browser's
  // timezone, with a UTC offset suffix (e.g. 2026-06-29 12:34:56 UTC-3).
  localtime = function(epoch) {
    var d = new Date(epoch * 1000);
    var off = -d.getTimezoneOffset();   // minutes east of UTC
    var ao = Math.abs(off);
    // No modulo here: livemap.js is served through the ESP template engine,
    // which treats a paired percent-sign as a placeholder and would eat it.
    var hh = Math.floor(ao / 60);
    var mm = ao - hh * 60;
    var tz = 'UTC' + (off < 0 ? '-' : '+') + hh + (mm ? ':' + az(mm) : '');
    return d.getFullYear() + '-' + az(d.getMonth()+1) + '-' + az(d.getDate()) + ' ' +
           az(d.getHours()) + ':' + az(d.getMinutes()) + ':' + az(d.getSeconds()) + ' ' + tz;
  };

  storage = (typeof(Storage) !== "undefined")?true:false;
  storage_write = function (data) {
    if (storage) {
      if (sessionStorage.sonde) {
        storage_data = JSON.parse(sessionStorage.sonde);
      } else {
        storage_data = [];
      }
      if (JSON.stringify(data) !=  JSON.stringify(storage_data[storage_data.length - 1])) {
        storage_data.push(data);
        sessionStorage.sonde = JSON.stringify(storage_data);
      }
    }
  };

  storage_read = function() {
    if (storage) {
      if (sessionStorage.sonde) {
        storage_data = JSON.parse(sessionStorage.sonde);
        return storage_data;
      }
    }
    return false;
  };

  storage_remove = function() {
    sessionStorage.removeItem('sonde');
  };

  session_storage = storage_read();
  if (session_storage) {
    session_storage.forEach(function(d) {
      // Rebuild the flight track keyed by sonde id, mirroring draw()'s plot guard
      // (valid lat/lon/alt, not a stale/kept position). Keying by id -- not a flat
      // dots.push() -- matches how draw() stores/reads the track (dots[data.id]),
      // so the restored trail is actually drawn.
      if (d.id && d.lat && d.lon && d.alt && !(d.validPos & 0x80)) {
        if (!dots[d.id]) { dots[d.id] = []; }
        dots[d.id].push([d.lat,d.lon,d.alt]);
      }
      session_storage_last = d;
    });
    for (var sid in dots) {
      if (!line[sid]) { line[sid] = L.polyline(dots[sid], {sondeid: sid}).addTo(map).on('click', line_click); }
    }
    if (session_storage_last) {
      // Prime lastframe so draw()'s plot guard (lastframe != 0, line ~197) passes on
      // this single restore call -- otherwise the marker is never created and the
      // sonde kept in session storage isn't shown after a reboot. Use -1 (nonzero and
      // != the frame's own vframe) so headtxt() still populates the header, then resets
      // lastframe to the real vframe.
      lastframe = -1;
      draw(session_storage_last);
    }
  }

  setInterval(get_data,1000);

});

L.Control.Button = L.Control.extend({
  onAdd: function (map) {
    var container = L.DomUtil.create('div', 'leaflet-bar leaflet-control');
    options = this.options;
    Object.keys(options).forEach(function(key) {
      this.link = L.DomUtil.create('a', '', container);
      this.link.text = options[key].text;
      this.link.href = options[key].href;
      this.link.id =  options[key].id;
    });

    this.options.position = this.options[0].position;
    return container;
  }
});


// https://github.com/makinacorpus/Leaflet.GeometryUtil/blob/master/src/leaflet.geometryutil.js#L682
// modified to fit
function bearing(latlng1, latlng2) {
    var rad = Math.PI / 180,
        lat1 = latlng1[0] * rad,
        lat2 = latlng2[0] * rad,
        lon1 = latlng1[1] * rad,
        lon2 = latlng2[1] * rad,
        y = Math.sin(lon2 - lon1) * Math.cos(lat2),
        x = Math.cos(lat1) * Math.sin(lat2) -
            Math.sin(lat1) * Math.cos(lat2) * Math.cos(lon2 - lon1);
    var bearing = ((Math.atan2(y, x) * 180 / Math.PI) + 360) % 360;
    bearing = bearing < 0 ? bearing-360 : bearing;
    return Math.round(bearing);
}
