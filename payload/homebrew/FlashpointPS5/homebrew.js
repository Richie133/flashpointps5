/* Flashpoint PS5 - entry for the websrv Homebrew Launcher.
 * Starts the Flashpoint PS5 payload in the background (it is a small web
 * server), which then opens the PS5 browser on its UI.
 * SPDX-License-Identifier: GPL-3.0-or-later */

async function main() {
    const PAYLOAD = window.workingDir + '/flashpoint-ps5.elf';
    const UI = 'http://127.0.0.1:22600';

    async function running() {
        try {
            const r = await fetch(UI + '/api/config');
            return r.ok;
        } catch (e) {
            return false;
        }
    }

    return {
        mainText: 'Flashpoint PS5',
        secondaryText: 'Browse and play Flashpoint Archive games',
        onclick: async () => {
            // Started twice, the payload just reopens the browser.
            await ApiClient.launchApp(PAYLOAD, '--open-browser', null, window.workingDir, true);
            return true;
        },
        options: [
            {
                text: 'Open on a phone or PC',
                onclick: async () => {
                    if (!(await running())) {
                        alert('Start Flashpoint PS5 first.');
                        return;
                    }
                    const cfg = await (await fetch(UI + '/api/config')).json();
                    alert('On a phone or PC on the same network, open:\nhttp://' + cfg.lanIp + ':' + cfg.uiPort);
                }
            },
            {
                text: 'Stop Flashpoint PS5',
                onclick: async () => {
                    if (!(await running())) {
                        alert('Flashpoint PS5 isn\'t running.');
                        return;
                    }
                    await fetch(UI + '/api/quit');
                    alert('Flashpoint PS5 has stopped.');
                }
            }
        ]
    };
}
