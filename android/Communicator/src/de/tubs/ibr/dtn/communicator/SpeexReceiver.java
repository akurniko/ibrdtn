package de.tubs.ibr.dtn.communicator;

import java.io.ByteArrayInputStream;
import java.io.Closeable;
import java.io.DataInputStream;
import java.io.IOException;

import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.util.Log;

import com.purplefrog.speexjni.FrequencyBand;
import com.purplefrog.speexjni.SpeexDecoder;

import de.tubs.ibr.dtn.streaming.Frame;

public class SpeexReceiver implements Closeable {
    
    private static final String TAG = "SpeexReceiver";
    
    private AudioTrack mAudioTrack = null;
    private SpeexDecoder mDecoder = null;
    
    public SpeexReceiver(byte[] meta) {
        // decode meta data
        DataInputStream meta_stream = new DataInputStream(new ByteArrayInputStream(meta));
        try {
            // read the header
            int version = meta_stream.readChar();
            
            // check audio version
            if (version != 1) throw new IOException("wrong version");
            
            // read frequency parameters
            int frequency = meta_stream.readInt();
            
            // read band parameters
            FrequencyBand band = readFrequencyBand(meta_stream);
            
            // create an decoder
            mDecoder = new SpeexDecoder(band);

            // get the minimum buffer size
            int buffer_size = 2 * AudioTrack.getMinBufferSize(frequency, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT);
            
            // create an AudioSink
            mAudioTrack = new AudioTrack(AudioManager.STREAM_MUSIC, frequency, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT, buffer_size, AudioTrack.MODE_STREAM);
            
            // start playback mode
            mAudioTrack.play();
        } catch (IOException e) {
            Log.e(TAG, "can not decode header data", e);
        } finally {
            try {
                meta_stream.close();
            } catch (IOException e) {
                Log.e(TAG, "failed to close meta data stream", e);
            }
        }
    }
    
    @Override
    public void close() {
        mAudioTrack.stop();
        mAudioTrack.release();
    }

    public void push(Frame frame) {
        short[] audio_data = mDecoder.decode(frame.data);
        mAudioTrack.write(audio_data, 0, audio_data.length);
    }
    
    private FrequencyBand readFrequencyBand(DataInputStream stream) throws IOException {
        // read band parameters
        switch (stream.readChar()) {
            default:
                return FrequencyBand.NARROW_BAND;
            case 1:
                return FrequencyBand.WIDE_BAND;
            case 2:
                return FrequencyBand.ULTRA_WIDE_BAND;
        }
    }
}
