import './style.css';
import { reactive, html } from '@arrow-js/core';
import { initDuckDB, loadDeltaSharingExtension, setupDeltaSharingFile, runQuery } from './duckdb';
import '@shoelace-style/shoelace/dist/components/button/button.js';
import '@shoelace-style/shoelace/dist/components/input/input.js';
import '@shoelace-style/shoelace/dist/components/textarea/textarea.js';

// Application State
const state = reactive({
  view: 'login', // 'login' | 'workspace'
  loading: false,
  error: '',
  sqlQuery: "SELECT * FROM delta_scan('profile.share') LIMIT 50;",
  queryResults: [] as any[],
  aiPrompt: '',
  aiSupported: 'ai' in window,
  manualEntryVisible: false,
  manualHost: '',
  manualToken: ''
});

// AI Query Generation using Chrome Browser Prompt API
async function generateSQL() {
  if (!state.aiSupported) return;
  state.loading = true;
  try {
    const ai = (window as any).ai;
    const session = await ai.createTextSession();
    
    // Construct the context prompt
    const prompt = `You are an expert SQL assistant. Given the following request, generate a DuckDB SQL query.
The table is accessed via delta_scan('profile.share').

Request: ${state.aiPrompt}
Current SQL: ${state.sqlQuery}

Return ONLY the raw SQL query without markdown blocks.`;

    const generated = await session.prompt(prompt);
    state.sqlQuery = generated.trim();
  } catch (err: any) {
    state.error = 'AI Error: ' + err.message;
  } finally {
    state.loading = false;
  }
}

// Execute SQL Query
async function executeSQL() {
  state.loading = true;
  state.error = '';
  try {
    const results = await runQuery(state.sqlQuery);
    state.queryResults = results;
  } catch (err: any) {
    state.error = 'SQL Error: ' + err.message;
  } finally {
    state.loading = false;
  }
}

// Handle File Upload (Login)
async function handleFileUpload(e: Event) {
  const target = e.target as HTMLInputElement;
  const file = target.files?.[0];
  if (!file) return;
  
  state.loading = true;
  state.error = '';
  
  try {
    const text = await file.text();
    await initDuckDB();
    await loadDeltaSharingExtension();
    await setupDeltaSharingFile(text);
    
    state.view = 'workspace';
  } catch (err: any) {
    state.error = 'Login Error: ' + err.message;
  } finally {
    state.loading = false;
  }
}

// Handle Manual Entry (Login)
async function handleManualEntry() {
  if (!state.manualHost || !state.manualToken) {
    state.error = 'Host and Token are required';
    return;
  }
  
  state.loading = true;
  state.error = '';
  
  try {
    const jsonConfig = JSON.stringify({
      shareCredentialsVersion: 1,
      endpoint: state.manualHost,
      bearerToken: state.manualToken
    });
    
    await initDuckDB();
    await loadDeltaSharingExtension();
    await setupDeltaSharingFile(jsonConfig);
    
    state.view = 'workspace';
  } catch (err: any) {
    state.error = 'Login Error: ' + err.message;
  } finally {
    state.loading = false;
  }
}

function clickUpload() {
  const el = document.getElementById('share-upload');
  if (el) el.click();
}

function updateAIPrompt(e: Event) {
  state.aiPrompt = (e.target as HTMLTextAreaElement).value;
}

function updateSQLQuery(e: Event) {
  state.sqlQuery = (e.target as HTMLTextAreaElement).value;
}

// Components
const LoginView = html`
  <div class="login-container">
    <div class="login-card">
      <h2 class="title"><span class="highlight-dot"></span> SYSTEM LOGIN // ACCESS PORTAL_</h2>
      
      <div style="margin-top: 2rem; display: flex; gap: 1rem;">
        <div style="flex: 1; border: 1px solid #333; padding: 1rem; cursor: pointer; text-align: center;"
             @click="${clickUpload}">
          [ UPLOAD .SHARE FILE ]
          <input type="file" id="share-upload" accept=".share,.json" style="display: none" @change="${handleFileUpload}" />
        </div>
        <div style="flex: 1; border: 1px solid #333; padding: 1rem; cursor: pointer; text-align: center;"
             @click="${() => state.manualEntryVisible = !state.manualEntryVisible}">
          [ MANUAL ENTRY (HOST & TOKEN) ]
        </div>
      </div>

      ${() => state.manualEntryVisible ? html`
        <div style="margin-top: 1rem; display: flex; flex-direction: column; gap: 1rem; border: 1px solid #333; padding: 1rem;">
          <sl-input placeholder="Endpoint URL (https://...)" @input="${(e: Event) => state.manualHost = (e.target as HTMLInputElement).value}"></sl-input>
          <sl-input type="password" placeholder="Bearer Token" @input="${(e: Event) => state.manualToken = (e.target as HTMLInputElement).value}"></sl-input>
          <sl-button variant="default" @click="${handleManualEntry}" ?loading="${state.loading}">[ CONNECT ]</sl-button>
        </div>
      ` : ''}

      ${() => state.loading && !state.manualEntryVisible ? html`<div style="margin-top: 1rem; color: #757575;">[ PROCESSING... ]</div>` : ''}
      ${() => state.error ? html`<div style="margin-top: 1rem; color: var(--highlight-red);">${state.error}</div>` : ''}
    </div>
  </div>
`;

const WorkspaceView = html`
  <div class="workspace-container">
    <div class="top-bar">
      <!-- AI Box -->
      <div class="ai-box">
        <div class="title" style="font-size: 0.8rem; margin-bottom: 0.5rem; color: #a3a3a3;">AI SQL ASSISTANT</div>
        ${() => state.aiSupported 
          ? html`
              <textarea class="sql-editor" placeholder="Ask AI to generate SQL..." @input="${updateAIPrompt}">${state.aiPrompt}</textarea>
              <sl-button variant="default" size="small" @click="${generateSQL}" ?loading="${state.loading}">
                Execute AI &rarr;
              </sl-button>
            `
          : html`
              <div style="color: #757575; font-size: 0.9rem;">[ AI Prompt API Not Supported in this Browser ]</div>
            `
        }
      </div>

      <!-- SQL Box -->
      <div class="sql-box">
        <div class="title" style="font-size: 0.8rem; margin-bottom: 0.5rem; color: #a3a3a3; display: flex; justify-content: space-between;">
          <span>SQL EDITOR</span>
          <sl-button variant="text" size="small" @click="${executeSQL}" ?loading="${state.loading}">[ RUN ]</sl-button>
        </div>
        <textarea class="sql-editor" @input="${updateSQLQuery}">${state.sqlQuery}</textarea>
      </div>
    </div>

    <!-- Results Box -->
    <div class="results-box">
      <div class="title" style="font-size: 0.8rem; margin-bottom: 0.5rem; color: #a3a3a3;">RESULTS (${() => state.queryResults.length} rows)</div>
      ${() => state.error ? html`<div style="color: var(--highlight-red);">${state.error}</div>` : ''}
      
      ${() => {
        if (state.queryResults.length === 0) return html`<div style="color: #757575;">[ NO DATA ]</div>`;
        
        const keys = Object.keys(state.queryResults[0]);
        return html`
          <table>
            <thead>
              <tr>${keys.map(k => html`<th>${k}</th>`)}</tr>
            </thead>
            <tbody>
              ${state.queryResults.map(row => html`
                <tr>${keys.map(k => html`<td>${row[k]}</td>`)}</tr>
              `)}
            </tbody>
          </table>
        `;
      }}
    </div>
  </div>
`;

// App Entry
const App = html`
  ${() => state.view === 'login' ? LoginView : WorkspaceView}
`;

const appEl = document.getElementById('app');
if (appEl) App(appEl);
