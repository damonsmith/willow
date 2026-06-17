import GObject from 'gi://GObject';
import St from 'gi://St';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PanelMenu from 'resource:///org/gnome/shell/ui/panelMenu.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

// Import modular components
import {ConfigManager} from './lib/ConfigManager.js';

// D-Bus interface XML
const VoiceAssistantIface = `
<node>
  <interface name="com.github.saim.Willow">
    <method name="SetMode">
      <arg direction="in" name="mode" type="s"/>
    </method>
    <method name="GetMode">
      <arg direction="out" name="mode" type="s"/>
    </method>
    <method name="GetStatus">
      <arg direction="out" name="status" type="a{sv}"/>
    </method>
    <method name="GetConfig">
      <arg direction="out" name="config" type="s"/>
    </method>
    <method name="UpdateConfig">
      <arg direction="in" name="config" type="s"/>
    </method>
    <method name="SetConfigValue">
      <arg direction="in" name="key" type="s"/>
      <arg direction="in" name="value" type="v"/>
    </method>
    <method name="Start"/>
    <method name="Stop"/>
    <method name="Restart"/>
    <method name="GetBuffer">
      <arg direction="out" name="buffer" type="s"/>
    </method>
    <signal name="ModeChanged">
      <arg name="new_mode" type="s"/>
      <arg name="old_mode" type="s"/>
    </signal>
    <signal name="BufferChanged">
      <arg name="buffer" type="s"/>
    </signal>
    <signal name="CommandExecuted">
      <arg name="command" type="s"/>
      <arg name="phrase" type="s"/>
      <arg name="confidence" type="d"/>
    </signal>
    <signal name="StatusChanged">
      <arg name="status" type="a{sv}"/>
    </signal>
    <signal name="Error">
      <arg name="message" type="s"/>
      <arg name="details" type="s"/>
    </signal>
    <signal name="Notification">
      <arg name="title" type="s"/>
      <arg name="message" type="s"/>
      <arg name="urgency" type="s"/>
    </signal>
    <signal name="ConfigChanged">
      <arg name="config" type="s"/>
    </signal>
    <property name="IsRunning" type="b" access="read"/>
    <property name="CurrentMode" type="s" access="read"/>
    <property name="CurrentBuffer" type="s" access="read"/>
    <property name="Version" type="s" access="read"/>
  </interface>
</node>`;

const VoiceAssistantProxy = Gio.DBusProxy.makeProxyWrapper(VoiceAssistantIface);

const VoiceAssistantIndicator = GObject.registerClass(
class VoiceAssistantIndicator extends PanelMenu.Button {
    _init(settings) {
        super._init(0.0, 'Willow');
        
        // Create panel UI
        this._box = new St.BoxLayout({
            style_class: 'panel-status-menu-box'
        });
        this.add_child(this._box);
        
        this._icon = new St.Icon({
            icon_name: 'microphone-sensitivity-medium-symbolic',
            style_class: 'system-status-icon'
        });
        this._box.add_child(this._icon);
        
        this._bufferLabel = new St.Label({
            text: '',
            style_class: 'willow-buffer-text',
            y_align: 2
        });
        this._box.add_child(this._bufferLabel);
        
        // State
        this._currentMode = 'normal';
        this._currentBuffer = '';
        this._isRunning = false;
        
        // Settings
        this._settings = settings;
        this._configManager = new ConfigManager(this._settings);
        
        // Setup D-Bus connection
        this._setupDBus();
        
        // Setup menu
        this._setupMenu();
        
        // Setup settings handlers
        this._setupSettingsHandlers();
        
        console.log('Willow: Extension initialized with D-Bus');
    }
    
    _setupDBus() {
        try {
            // Create proxy to the D-Bus service
            this._proxy = new VoiceAssistantProxy(
                Gio.DBus.session,
                'com.github.saim.Willow',
                '/com/github/saim/VoiceAssistant',
                (proxy, error) => {
                    if (error) {
                        console.error('Willow: D-Bus connection error:', error);
                        console.error('Failed to connect to Willow service');
                        return;
                    }
                    
                    this._onDBusConnected();
                }
            );
            
        } catch (e) {
            console.error('Willow: Failed to create D-Bus proxy:', e);
        }
    }
    
    _onDBusConnected() {
        console.log('Willow: Connected to D-Bus service');
        
        // Connect to signals
        this._proxy.connectSignal('ModeChanged', (proxy, sender, [newMode, oldMode]) => {
            this._onModeChanged(newMode, oldMode);
        });
        
        this._proxy.connectSignal('BufferChanged', (proxy, sender, [buffer]) => {
            this._onBufferChanged(buffer);
        });
        
        this._proxy.connectSignal('CommandExecuted', (proxy, sender, [command, phrase, confidence]) => {
            this._onCommandExecuted(command, phrase, confidence);
        });
        
        this._proxy.connectSignal('StatusChanged', (proxy, sender, [status]) => {
            this._onStatusChanged(status);
        });
        
        this._proxy.connectSignal('Error', (proxy, sender, [message, details]) => {
            this._onError(message, details);
        });
        
        this._proxy.connectSignal('Notification', (proxy, sender, [title, message, urgency]) => {
            this._onNotification(title, message, urgency);
        });
        
        // Get initial status and auto-start if not running
        this._updateStatus();
        
        // Auto-start the service if it's not already running
        try {
            this._proxy.IsRunning = false; // Force initial check
            GLib.timeout_add(GLib.PRIORITY_DEFAULT, 500, () => {
                this._proxy.GetStatusRemote((result, error) => {
                    if (!error && result) {
                        const [status] = result;
                        if (!status.is_running || !status.is_running.unpack()) {
                            console.log('Willow: Auto-starting service');
                            this._startService();
                        }
                    }
                });
                return GLib.SOURCE_REMOVE;
            });
        } catch (e) {
            console.log('Willow: Auto-start check failed:', e);
        }
        
        // Poll status periodically
        this._statusTimer = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 2, () => {
            this._updateStatus();
            return GLib.SOURCE_CONTINUE;
        });
    }
    
    _setupMenu() {
        // Mode display
        this._modeItem = new PopupMenu.PopupMenuItem(`Mode: NORMAL`, {
            reactive: false
        });
        this.menu.addMenuItem(this._modeItem);
        
        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
        
        // Service control
        this._serviceStatusItem = new PopupMenu.PopupMenuItem('Service: Checking...', {
            reactive: false
        });
        this.menu.addMenuItem(this._serviceStatusItem);
        
        this._modelInfoItem = new PopupMenu.PopupMenuItem('Model: ...', {
            reactive: false
        });
        this.menu.addMenuItem(this._modelInfoItem);
        
        this._startItem = new PopupMenu.PopupMenuItem('Start Service');
        this._startItem.connect('activate', () => this._startService());
        this.menu.addMenuItem(this._startItem);

        this._stopItem = new PopupMenu.PopupMenuItem('Stop Service');
        this._stopItem.connect('activate', () => this._stopService());
        this.menu.addMenuItem(this._stopItem);

        this._restartItem = new PopupMenu.PopupMenuItem('Restart Service');
        this._restartItem.connect('activate', () => this._restartService());
        this.menu.addMenuItem(this._restartItem);

        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
        
        // Mode switching
        this._normalModeItem = new PopupMenu.PopupMenuItem('Switch to Normal Mode');
        this._normalModeItem.connect('activate', () => this._setMode('normal'));
        this.menu.addMenuItem(this._normalModeItem);
        
        this._commandModeItem = new PopupMenu.PopupMenuItem('Switch to Command Mode');
        this._commandModeItem.connect('activate', () => this._setMode('command'));
        this.menu.addMenuItem(this._commandModeItem);
        
        this._typingModeItem = new PopupMenu.PopupMenuItem('Switch to Typing Mode');
        this._typingModeItem.connect('activate', () => this._setMode('typing'));
        this.menu.addMenuItem(this._typingModeItem);
        
        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
        
        // Buffer display
        this._bufferItem = new PopupMenu.PopupMenuItem('Buffer: (empty)', {
            reactive: false
        });
        this.menu.addMenuItem(this._bufferItem);
        
        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
        
        // Smart Workflows info (in command mode)
        this._smartInfoItem = new PopupMenu.PopupMenuItem('Smart: Say "open [app]" or "search [engine] for [query]"', {
            reactive: false,
            style_class: 'willow-smart-info'
        });
        this.menu.addMenuItem(this._smartInfoItem);
        this._smartInfoItem.visible = false; // Only show in command mode
        
        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
        
        // Preferences
        this._prefsItem = new PopupMenu.PopupMenuItem('Preferences');
        this._prefsItem.connect('activate', () => {
            try {
                GLib.spawn_command_line_async('gnome-extensions prefs willow@saim');
            } catch (e) {
                console.error('Willow: Error opening preferences:', e);
            }
        });
        this.menu.addMenuItem(this._prefsItem);
    }
    
    _setupSettingsHandlers() {
        // When settings change, sync to D-Bus service
        const syncableKeys = ['hotword', 'command-threshold', 'processing-interval', 'gpu-acceleration'];
        
        syncableKeys.forEach(key => {
            this._settings.connect(`changed::${key}`, () => {
                this._syncSettingsToService();
            });
        });
    }
    
    _syncSettingsToService() {
        if (!this._proxy) return;
        
        try {
            const hotword = this._settings.get_string('hotword');
            const threshold = this._settings.get_double('command-threshold') / 100.0;
            const interval = this._settings.get_double('processing-interval');
            const gpuAcceleration = this._settings.get_boolean('gpu-acceleration');
            
            this._proxy.SetConfigValueRemote('hotword', new GLib.Variant('s', hotword));
            this._proxy.SetConfigValueRemote('command_threshold', new GLib.Variant('d', threshold));
            this._proxy.SetConfigValueRemote('processing_interval', new GLib.Variant('d', interval));
            this._proxy.SetConfigValueRemote('gpu_acceleration', new GLib.Variant('b', gpuAcceleration));
            
            console.log('Willow: Settings synced to service');
        } catch (e) {
            console.error('Willow: Error syncing settings:', e);
        }
    }
    
    // D-Bus method wrappers
    
    _setMode(mode) {
        if (!this._proxy) {
            console.error('Willow: Service not connected');
            return;
        }
        
        try {
            this._proxy.SetModeRemote(mode, (result, error) => {
                if (error) {
                    console.error('Willow: SetMode error:', error);
                    console.error('Failed to change mode');
                }
            });
        } catch (e) {
            console.error('Willow: SetMode exception:', e);
        }
    }
    
    _startService() {
        if (!this._proxy) {
            console.error('Willow: Service not connected');
            return;
        }
        
        try {
            this._proxy.StartRemote((result, error) => {
                if (error) {
                    console.error('Willow: Start error:', error);
                    console.error('Failed to start service');
                }
            });
        } catch (e) {
            console.error('Willow: Start exception:', e);
        }
    }
    
    _stopService() {
        if (!this._proxy) return;
        
        try {
            this._proxy.StopRemote((result, error) => {
                if (error) {
                    console.error('Willow: Stop error:', error);
                }
            });
        } catch (e) {
            console.error('Willow: Stop exception:', e);
        }
    }
    
    _restartService() {
        if (!this._proxy) return;
        
        try {
            this._proxy.RestartRemote((result, error) => {
                if (error) {
                    console.error('Willow: Restart error:', error);
                }
            });
        } catch (e) {
            console.error('Willow: Restart exception:', e);
        }
    }
    
    _updateStatus() {
        if (!this._proxy) return;
        
        try {
            this._proxy.GetStatusRemote((result, error) => {
                if (error) {
                    console.error('Willow: GetStatus error:', error);
                    return;
                }
                
                if (result && result[0]) {
                    const status = result[0];
                    this._onStatusChanged(status);
                }
            });
        } catch (e) {
            console.error('Willow: GetStatus exception:', e);
        }
    }
    
    // Signal handlers
    
    _onModeChanged(newMode, oldMode) {
        this._currentMode = newMode;
        this._updateDisplay();
        
        // Mode changes are shown in the panel, no notification needed
        console.log(`Willow: Mode changed from ${oldMode} to ${newMode}`);
    }
    
    _onBufferChanged(buffer) {
        this._currentBuffer = buffer;
        this._updateDisplay();
    }
    
    _onCommandExecuted(command, phrase, confidence) {
        console.log(`Willow: Command executed: ${phrase} (${(confidence * 100).toFixed(1)}%)`);
        
        // Command execution logged to console only, no notifications
    }
    
    _onStatusChanged(status) {
        if (status.is_running !== undefined) {
            this._isRunning = status.is_running.unpack();
        }
        if (status.current_mode !== undefined) {
            this._currentMode = status.current_mode.unpack();
        }
        if (status.current_buffer !== undefined) {
            this._currentBuffer = status.current_buffer.unpack();
        }
        if (status.gpu_enabled !== undefined) {
            this._gpuEnabled = status.gpu_enabled.unpack();
        }
        if (status.whisper_model !== undefined) {
            this._whisperModel = status.whisper_model.unpack();
        }
        
        this._updateDisplay();
    }
    
    _onError(message, details) {
        console.error('Willow:', message, details);
    }
    
    _onNotification(title, message, urgency) {
        // Notifications disabled - service notifications are logged only
        console.log(`Willow notification: ${title} - ${message}`);
    }
    
    // UI updates
    
    _updateDisplay() {
        // Update icon based on mode
        let iconName = 'microphone-sensitivity-medium-symbolic';
        let iconStyle = '';
        
        if (!this._isRunning) {
            iconName = 'microphone-disabled-symbolic';
        } else if (this._currentMode === 'command') {
            iconName = 'microphone-sensitivity-high-symbolic';
            iconStyle = 'color: #ff4444;'; // Red for command mode
        } else if (this._currentMode === 'typing') {
            iconName = 'input-keyboard-symbolic';
        }
        
        this._icon.icon_name = iconName;
        if (iconStyle) {
            this._icon.style = iconStyle;
        } else {
            this._icon.style = '';
        }
        
        // Update buffer text
        const maxBufferLength = 50;
        let bufferText = this._currentBuffer;
        if (bufferText.length > maxBufferLength) {
            bufferText = '...' + bufferText.substring(bufferText.length - maxBufferLength);
        }
        this._bufferLabel.text = bufferText ? ` ${bufferText}` : '';
        
        // Update smart info visibility (only show in command mode)
        if (this._smartInfoItem) {
            this._smartInfoItem.visible = (this._currentMode === 'command');
        }
        
        // Update menu items
        if (this._modeItem) {
            this._modeItem.label.text = `Mode: ${this._currentMode.toUpperCase()}`;
        }
        
        if (this._bufferItem) {
            this._bufferItem.label.text = this._currentBuffer 
                ? `Buffer: ${this._currentBuffer}` 
                : 'Buffer: (empty)';
        }
        
        if (this._serviceStatusItem) {
            this._serviceStatusItem.label.text = this._isRunning 
                ? 'Service: Running' 
                : 'Service: Stopped';
        }
        
        if (this._modelInfoItem && this._whisperModel) {
            const model = this._whisperModel.replace('ggml-', '').replace('.bin', '');
            const gpu = this._gpuEnabled ? 'GPU' : 'CPU';
            this._modelInfoItem.label.text = `Model: ${model} (${gpu})`;
        }
        
        // Update menu button sensitivity
        if (this._startItem) this._startItem.setSensitive(!this._isRunning);
        if (this._stopItem) this._stopItem.setSensitive(this._isRunning);
        if (this._restartItem) this._restartItem.setSensitive(this._isRunning);
    }
    
    destroy() {
        // Clean up timers
        if (this._statusTimer) {
            GLib.source_remove(this._statusTimer);
            this._statusTimer = null;
        }
        
        // Clean up D-Bus proxy
        if (this._proxy) {
            this._proxy = null;
        }
        
        super.destroy();
    }
});

export default class VoiceAssistantExtension extends Extension {
    constructor(metadata) {
        super(metadata);
        this._indicator = null;
    }

    enable() {
        console.log('Willow: Enabling extension');
        const settings = this.getSettings();
        this._indicator = new VoiceAssistantIndicator(settings);
        Main.panel.addToStatusArea('willow', this._indicator);
    }

    disable() {
        console.log('Willow: Disabling extension');
        if (this._indicator) {
            this._indicator.destroy();
            this._indicator = null;
        }
    }
}
