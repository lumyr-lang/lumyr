// T2: Node.js 客户端流式上传文件到 lumin 服务端
// 使用 createReadStream 边读边发（64KB 块 + drain 背压），内存占用恒定
// 使用真正的换行符 \n（ASCII 10）
const net = require('net');
const fs = require('fs');

const PORT = 19200;
const HOST = '127.0.0.1';
const CHUNK = 65536;

// 等服务端一行 READY
function waitReady(conn) {
    return new Promise((resolve, reject) => {
        let buf = '';
        const onData = (chunk) => {
            buf += chunk.toString();
            if (buf.includes('\n')) {
                conn.removeListener('data', onData);
                const line = buf.slice(0, buf.indexOf('\n'));
                if (line === 'READY') { resolve(); }
                else { reject(new Error(`Expected READY, got [${line}]`)); }
            }
        };
        conn.on('data', onData);
    });
}

// 流式发送：header → READY → 文件流分块写入（背压）→ DONE
function sendFileStreaming(conn, filepath, size) {
    conn.write(`R:${size}\n`);
    return waitReady(conn).then(() => {
        return new Promise((resolve, reject) => {
            let sent = 0;
            const stream = fs.createReadStream(filepath, { highWaterMark: CHUNK });
            stream.on('data', (chunk) => {
                // 只发送前 size 字节
                let piece = chunk;
                if (sent + chunk.length > size) {
                    piece = chunk.slice(0, size - sent);
                }
                sent += piece.length;
                const ok = conn.write(piece);
                if (!ok) {
                    stream.pause();
                    conn.once('drain', () => stream.resume());
                }
                if (sent >= size) {
                    stream.destroy();
                    conn.write('DONE\n');
                    resolve();
                }
            });
            stream.on('error', reject);
        });
    });
}

function readResult(conn) {
    return new Promise((resolve, reject) => {
        let resp = '';
        const onData = (chunk) => {
            resp += chunk.toString();
            if (resp.startsWith('OK ') || resp.startsWith('ERR ')) {
                conn.removeListener('data', onData);
                conn.end();
                resolve(resp.trim());
            }
        };
        conn.on('data', onData);
        conn.on('error', reject);
    });
}

async function testUpload(size, label) {
    const filepath = size > 1024 * 1024 ? '/tmp/lumin_probe_data_10m.bin' : '/tmp/lumin_probe_data_1m.bin';
    const conn = net.createConnection(PORT, HOST);
    conn.setTimeout(60000);
    await new Promise((resolve, reject) => {
        conn.once('connect', resolve);
        conn.once('error', reject);
    });
    // READY/数据阶段由 sendFileStreaming 独占 data 监听，避免响应被污染
    await sendFileStreaming(conn, filepath, size);
    const resp = await readResult(conn);
    const expected = `OK ${size}`;
    if (resp !== expected) {
        throw new Error(`[${label}] expected [${expected}], got [${resp}]`);
    }
    console.log(`[${label}] PASS: ${resp}`);
}

async function main() {
    try {
        await testUpload(1024 * 1024, 'T2-Node-1MB');
        await testUpload(10 * 1024 * 1024, 'T2-Node-10MB');
        console.log('T2 Node.js 流式上传 ALL PASS');
        process.exit(0);
    } catch (e) {
        console.error('T2 FAIL:', e.message);
        process.exit(1);
    }
}

main();
