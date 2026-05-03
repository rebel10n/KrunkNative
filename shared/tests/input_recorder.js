const resetMeleeAnim = Symbol('resetMeleeAnim');
const active = Symbol('active');
const wasActive = Symbol('wasActive');

let recording = {
    active: false,
    ticks: [],
};

let notification = document.createElement('div');
let anim = document.createElement('style');

notification.style.cssText = `
    position: absolute;
    z-index: 9999;
    top: 10px;
    right: 10px;
    transform: translateX(calc(100% + 20px));
    padding: 10px;
    margin: 10px;
    box-sizing: border-box;
    border-radius: 5px;
    background: #0005;
    color: #fff;
`;

anim.textContent = `
    @keyframes slide {
        0%, 100% {
            transform: translateX(calc(100% + 20px));
        }
    
        10%, 90% {
            transform: translateX(0);
        }
    }
    
    .anim {
        animation: slide 2s forwards;
    }
`;

document.body.append(notification, anim);

function notify(message) {
    notification.innerHTML = message;
    notification.className = '';

    void notification.offsetHeight;
    notification.className = 'anim';
}

function saveRecording() {
    navigator.clipboard.writeText(JSON.stringify(recording.ticks)).then(() => {
        notify(`Saved <span style="color: turquoise">${recording.ticks.length}</span> ticks to clipboard!`);
        recording.ticks = [];
    }).catch((error) => {
        notify(`Failed to save recording to clipboard: <span style="color: crimson"${error.message}</span>`);
    });
}

function record(original, ...args) {
    const player = this;
    const [input, game, recon, moveLock] = args;

    if (!player[wasActive]) {
        recording.active = true;
        player[wasActive] = true;

        notify('Input recording started!');
    }

    if (recording.active) recording.ticks.push([
        [input[1], input[2], input[3], input[4], input[7], input[8], input[6]],
        [player.x, player.y, player.z, player.velX, player.velY, player.velZ, player.rampFix],
    ]);

    return original.apply(this, args);
}

function onPlayer(player, key) {
    const keys = Object.keys(player);
    const procInputs = keys[key];

    const original = player[procInputs];

    player[procInputs] = record.bind(player, original);
    player[active] = player.active;

    Object.defineProperties(player, {
        active: {
            get: () => player[active],
            set: (v) => {
                if (!v) player[wasActive] = false;
                player[active] = v;

                return true;
            },

            configurable: true,
            enumerable: false,
        },
    });
}

Object.defineProperties(Object.prototype, {
    resetMeleeAnim: {
        get: function () {
            return this[resetMeleeAnim];
        },

        set: function (v) {
            setTimeout(onPlayer.bind(null, this, Object.keys(this).length));
            this[resetMeleeAnim] = v;

            return true;
        },

        configurable: true,
        enumerable: false,
    },
});

document.addEventListener('keydown', ({ key}) => {
    if (key.toLowerCase() !== 'c') return;

    if (!recording.active) {
        notify('Recording is currently inactive; please respawn/enter the game.');
        return;
    }

    recording.active = false;
    saveRecording();
});
