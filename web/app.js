async function fetchJson(path){
  const res = await fetch(path);
  if(!res.ok) throw new Error(res.statusText);
  return res.json();
}

async function initDashboard(){
  try{
    const [cfg,diag] = await Promise.all([fetchJson('/api/config'),fetchJson('/api/diag')]);
    document.getElementById('panel-name').textContent = cfg.panel_name || 'Panel';
    
    let statusHtml = '<div class="status">';
    statusHtml += `<div class="status-item"><span class="status-dot ${diag.mqtt_connected?'online':'offline'}"></span> MQTT: ${diag.mqtt_connected?'Online':'Offline'}</div>`;
    statusHtml += `<div class="status-item"><span class="status-dot ${diag.eth_connected?'online':'offline'}"></span> ETH: ${diag.eth_connected?'Online':'Offline'}</div>`;
    statusHtml += `<div class="status-item"><span class="status-dot ${diag.wifi_connected?'online':'offline'}"></span> Wi-Fi: ${diag.wifi_connected?'Online':'Offline'}</div>`;
    statusHtml += '</div>';
    document.getElementById('status')?.remove();
    const statusDiv = document.createElement('div');
    statusDiv.id = 'status';
    statusDiv.innerHTML = statusHtml;
    document.querySelector('main').insertBefore(statusDiv, document.querySelector('main').firstChild);
    
    const banksDiv = document.getElementById('banks');
    banksDiv.innerHTML = '';
    (cfg.banks||[]).forEach((b,idx)=>{
      const bd = document.createElement('div');
      bd.className = 'bank-section';
      bd.innerHTML = `<h3>Bank ${idx+1}: ${b.bank_name}</h3><div class="button-list">`;
      b.buttons.forEach((btn,i)=> {
        bd.querySelector('.button-list').innerHTML += `<div class="button-item"><strong>${i+1}. ${btn.display_name}</strong>${btn.mqtt_topic}</div>`;
      });
      bd.querySelector('.button-list').innerHTML += '</div>';
      banksDiv.appendChild(bd);
    });
  }catch(e){console.error(e)}
}

async function initConfig(){
  try{
    const cfg = await fetchJson('/api/config');
    const form = document.getElementById('config-form');
    form.elements['panel_name'].value = cfg.panel_name || '';
    form.elements['mqtt.broker_ip'].value = cfg.mqtt.broker_ip || '';
    form.elements['mqtt.port'].value = cfg.mqtt.port || '';
    form.elements['mqtt.username'].value = cfg.mqtt.username || '';
    form.elements['mqtt.password'].value = cfg.mqtt.password || '';

    form.addEventListener('submit', async (ev)=>{
      ev.preventDefault();
      const alertDiv = document.getElementById('alert');
      try {
        const body = {
          panel_name: form.elements['panel_name'].value,
          mqtt: {
            broker_ip: form.elements['mqtt.broker_ip'].value,
            port: form.elements['mqtt.port'].value,
            username: form.elements['mqtt.username'].value,
            password: form.elements['mqtt.password'].value
          }
        };
        const res = await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
        if(res.ok){
          alertDiv.innerHTML = '<div class="alert success">✓ Configuration saved successfully</div>';
          setTimeout(() => alertDiv.innerHTML = '', 3000);
        }else{
          alertDiv.innerHTML = '<div class="alert error">✗ Failed to save configuration</div>';
        }
      }catch(e){
        alertDiv.innerHTML = '<div class="alert error">✗ Error: '+e.message+'</div>';
      }
    });
  }catch(e){console.error(e)}
}

async function initDiag(){
  try{
    const d = await fetchJson('/api/diag');
    document.getElementById('diag').textContent = JSON.stringify(d,null,2);
  }catch(e){console.error(e)}
}

// Router: call init based on path
if (location.pathname === '/' || location.pathname === '/dashboard') initDashboard();
if (location.pathname === '/config') initConfig();
if (location.pathname === '/diag') initDiag();
