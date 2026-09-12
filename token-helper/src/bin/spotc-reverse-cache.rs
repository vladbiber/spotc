// Decode one Spotify track to raw s16le stereo without registering another
// Connect device.  The pipe sink is intentionally file-backed and therefore
// runs as fast as download/decoding allows instead of at wall-clock speed.
use std::{env, process::exit};

use librespot_core::{cache::Cache, config::SessionConfig, session::Session, SpotifyUri};
use librespot_playback::{
    audio_backend,
    config::{AudioFormat, Bitrate, PlayerConfig},
    mixer::NoOpVolume,
    player::{Player, PlayerEvent},
};

#[tokio::main(flavor = "current_thread")]
async fn main() {
    let mut args = env::args().skip(1);
    let (Some(cache_dir), Some(uri), Some(output)) =
        (args.next(), args.next(), args.next())
    else {
        eprintln!("usage: spotc-reverse-cache <librespot-cache> <track-uri> <output> [bitrate]");
        exit(1);
    };
    let bitrate = match args.next().as_deref().unwrap_or("320") {
        "96" => Bitrate::Bitrate96,
        "160" => Bitrate::Bitrate160,
        "320" => Bitrate::Bitrate320,
        value => {
            eprintln!("invalid bitrate: {value}");
            exit(1);
        }
    };

    let cache = Cache::new(Some(cache_dir.as_str()), None::<&str>, None::<&str>, None)
        .unwrap_or_else(|e| {
            eprintln!("cache: {e}");
            exit(2);
        });
    let credentials = cache.credentials().unwrap_or_else(|| {
        eprintln!("no cached librespot credentials");
        exit(3);
    });
    let track = SpotifyUri::from_uri(&uri).unwrap_or_else(|e| {
        eprintln!("track URI: {e}");
        exit(4);
    });
    if !track.is_playable() {
        eprintln!("URI is not a playable track");
        exit(4);
    }

    let session = Session::new(SessionConfig::default(), None);
    session.connect(credentials, false).await.unwrap_or_else(|e| {
        eprintln!("connect: {e}");
        exit(5);
    });

    let mut config = PlayerConfig::default();
    config.bitrate = bitrate;
    config.gapless = false;
    config.ditherer = None;
    let backend = audio_backend::find(Some("pipe".to_owned())).expect("pipe backend");
    let player = Player::new(config, session, Box::new(NoOpVolume), move || {
        backend(Some(output), AudioFormat::S16)
    });
    let mut events = player.get_player_event_channel();
    player.load(track, true, 0);

    while let Some(event) = events.recv().await {
        match event {
            PlayerEvent::EndOfTrack { .. } => {
                player.stop();
                return;
            }
            PlayerEvent::Unavailable { .. } => {
                eprintln!("track unavailable");
                player.stop();
                exit(6);
            }
            _ => {}
        }
    }
    eprintln!("decoder stopped before end of track");
    exit(7);
}
